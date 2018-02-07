/*
 *
 * (C) COPYRIGHT 2010-2016 ARM Limited. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */



/*
 * Base kernel job manager APIs
 */

#include <mali_kbase.h>
#include <mali_kbase_config.h>
#include <mali_midg_regmap.h>
#if defined(CONFIG_MALI_GATOR_SUPPORT)
#include <mali_kbase_gator.h>
#endif
#include <mali_kbase_tlstream.h>
#include <mali_kbase_hw.h>
#include <mali_kbase_config_defaults.h>
#include <mali_kbase_hwaccess_jm.h>
#include <backend/gpu/mali_kbase_device_internal.h>
#include <backend/gpu/mali_kbase_irq_internal.h>
#include <backend/gpu/mali_kbase_js_affinity.h>
#include <backend/gpu/mali_kbase_jm_internal.h>

#define beenthere(kctx, f, a...) \
			dev_dbg(kctx->kbdev->dev, "%s:" f, __func__, ##a)

#if KBASE_GPU_RESET_EN
static void kbasep_try_reset_gpu_early(struct kbase_device *kbdev);
static void kbasep_reset_timeout_worker(struct work_struct *data);
static enum hrtimer_restart kbasep_reset_timer_callback(struct hrtimer *timer);
#endif /* KBASE_GPU_RESET_EN */

static inline int kbasep_jm_is_js_free(struct kbase_device *kbdev, int js,
						struct kbase_context *kctx)
{
	return !kbase_reg_read(kbdev, JOB_SLOT_REG(js, JS_COMMAND_NEXT), kctx);
}

void kbase_job_hw_submit(struct kbase_device *kbdev,
				struct kbase_jd_atom *katom,
				int js)
{
	struct kbase_context *kctx;
	u32 cfg;
	u64 jc_head = katom->jc;

	KBASE_DEBUG_ASSERT(kbdev);
	KBASE_DEBUG_ASSERT(katom);

	kctx = katom->kctx;

	/* Command register must be available */
	KBASE_DEBUG_ASSERT(kbasep_jm_is_js_free(kbdev, js, kctx));
	/* Affinity is not violating */
	kbase_js_debug_log_current_affinities(kbdev);
	KBASE_DEBUG_ASSERT(!kbase_js_affinity_would_violate(kbdev, js,
							katom->affinity));

	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_HEAD_NEXT_LO),
						jc_head & 0xFFFFFFFF, kctx);
	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_HEAD_NEXT_HI),
						jc_head >> 32, kctx);

	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_AFFINITY_NEXT_LO),
					katom->affinity & 0xFFFFFFFF, kctx);
	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_AFFINITY_NEXT_HI),
					katom->affinity >> 32, kctx);

	/* start MMU, medium priority, cache clean/flush on end, clean/flush on
	 * start */
	cfg = kctx->as_nr;
	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_FLUSH_REDUCTION))
		cfg |= JS_CONFIG_ENABLE_FLUSH_REDUCTION;

#ifndef CONFIG_MALI_COH_GPU
	cfg |= JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE;
	cfg |= JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE;
#endif

	cfg |= JS_CONFIG_START_MMU;
	cfg |= JS_CONFIG_THREAD_PRI(8);

	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_PROTECTED_MODE) &&
		(katom->atom_flags & KBASE_KATOM_FLAG_SECURE))
		cfg |= JS_CONFIG_DISABLE_DESCRIPTOR_WR_BK;

	if (kbase_hw_has_feature(kbdev,
				BASE_HW_FEATURE_JOBCHAIN_DISAMBIGUATION)) {
		if (!kbdev->hwaccess.backend.slot_rb[js].job_chain_flag) {
			cfg |= JS_CONFIG_JOB_CHAIN_FLAG;
			katom->atom_flags |= KBASE_KATOM_FLAGS_JOBCHAIN;
			kbdev->hwaccess.backend.slot_rb[js].job_chain_flag =
								true;
		} else {
			katom->atom_flags &= ~KBASE_KATOM_FLAGS_JOBCHAIN;
			kbdev->hwaccess.backend.slot_rb[js].job_chain_flag =
								false;
		}
	}

	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_CONFIG_NEXT), cfg, kctx);

	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_FLUSH_REDUCTION))
		kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_FLUSH_ID_NEXT),
				katom->flush_id, kctx);

	/* Write an approximate start timestamp.
	 * It's approximate because there might be a job in the HEAD register.
	 * In such cases, we'll try to make a better approximation in the IRQ
	 * handler (up to the KBASE_JS_IRQ_THROTTLE_TIME_US). */
	katom->start_timestamp = ktime_get();

	/* GO ! */
	dev_dbg(kbdev->dev, "JS: Submitting atom %p from ctx %p to js[%d] with head=0x%llx, affinity=0x%llx",
				katom, kctx, js, jc_head, katom->affinity);

	KBASE_TRACE_ADD_SLOT_INFO(kbdev, JM_SUBMIT, kctx, katom, jc_head, js,
							(u32) katom->affinity);

#if defined(CONFIG_MALI_GATOR_SUPPORT)
	kbase_trace_mali_job_slots_event(
				GATOR_MAKE_EVENT(GATOR_JOB_SLOT_START, js),
				kctx, kbase_jd_atom_id(kctx, katom));
#endif
	kbase_tlstream_tl_attrib_atom_config(katom, jc_head,
			katom->affinity, cfg);
	kbase_tlstream_tl_ret_ctx_lpu(
		kctx,
		&kbdev->gpu_props.props.raw_props.js_features[
			katom->slot_nr]);
	kbase_tlstream_tl_ret_atom_as(katom, &kbdev->as[kctx->as_nr]);
	kbase_tlstream_tl_ret_atom_lpu(
			katom,
			&kbdev->gpu_props.props.raw_props.js_features[js],
			"ctx_nr,atom_nr");
#ifdef CONFIG_GPU_TRACEPOINTS
	if (!kbase_backend_nr_atoms_submitted(kbdev, js)) {
		/* If this is the only job on the slot, trace it as starting */
		char js_string[16];

		trace_gpu_sched_switch(
				kbasep_make_job_slot_string(js, js_string),
				ktime_to_ns(katom->start_timestamp),
				(u32)katom->kctx->id, 0, katom->work_id);
		kbdev->hwaccess.backend.slot_rb[js].last_context = katom->kctx;
	}
#endif
	kbase_timeline_job_slot_submit(kbdev, kctx, katom, js);

	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_COMMAND_NEXT),
						JS_COMMAND_START, katom->kctx);
}

/**
 * kbasep_job_slot_update_head_start_timestamp - Update timestamp
 * @kbdev: kbase device
 * @js: job slot
 * @end_timestamp: timestamp
 *
 * Update the start_timestamp of the job currently in the HEAD, based on the
 * fact that we got an IRQ for the previous set of completed jobs.
 *
 * The estimate also takes into account the %KBASE_JS_IRQ_THROTTLE_TIME_US and
 * the time the job was submitted, to work out the best estimate (which might
 * still result in an over-estimate to the calculated time spent)
 */
static void kbasep_job_slot_update_head_start_timestamp(
						struct kbase_device *kbdev,
						int js,
						ktime_t end_timestamp)
{
	if (kbase_backend_nr_atoms_on_slot(kbdev, js) > 0) {
		struct kbase_jd_atom *katom;
		ktime_t new_timestamp;
		ktime_t timestamp_diff;
		/* The atom in the HEAD */
		katom = kbase_gpu_inspect(kbdev, js, 0);

		KBASE_DEBUG_ASSERT(katom != NULL);

		/* Account for any IRQ Throttle time - makes an overestimate of
		 * the time spent by the job */
		new_timestamp = ktime_sub_ns(end_timestamp,
					KBASE_JS_IRQ_THROTTLE_TIME_US * 1000);
		timestamp_diff = ktime_sub(new_timestamp,
							katom->start_timestamp);
		if (ktime_to_ns(timestamp_diff) >= 0) {
			/* Only update the timestamp if it's a better estimate
			 * than what's currently stored. This is because our
			 * estimate that accounts for the throttle time may be
			 * too much of an overestimate */
			katom->start_timestamp = new_timestamp;
		}
	}
}

/**
 * kbasep_trace_tl_nret_atom_lpu - Call nret_atom_lpu timeline tracepoint
 * @kbdev: kbase device
 * @i: job slot
 *
 * Get kbase atom by calling kbase_gpu_inspect for given job slot.
 * Then use obtained katom and name of slot associated with the given
 * job slot number in tracepoint call to the instrumentation module
 * informing that given atom is no longer executed on given lpu (job slot).
 */
static void kbasep_trace_tl_nret_atom_lpu(struct kbase_device *kbdev, int i)
{
	struct kbase_jd_atom *katom = kbase_gpu_inspect(kbdev, i, 0);

	kbase_tlstream_tl_nret_atom_lpu(katom,
		&kbdev->gpu_props.props.raw_props.js_features[i]);
}

void kbase_job_done(struct kbase_device *kbdev, u32 done)
{
	unsigned long flags;
	int i;
	u32 count = 0;
	ktime_t end_timestamp = ktime_get();
	struct kbasep_js_device_data *js_devdata;

	KBASE_DEBUG_ASSERT(kbdev);
	js_devdata = &kbdev->js_data;

	KBASE_TRACE_ADD(kbdev, JM_IRQ, NULL, NULL, 0, done);

	memset(&kbdev->slot_submit_count_irq[0], 0,
					sizeof(kbdev->slot_submit_count_irq));

	/* write irq throttle register, this will prevent irqs from occurring
	 * until the given number of gpu clock cycles have passed */
	{
		int irq_throttle_cycles =
				atomic_read(&kbdev->irq_throttle_cycles);

		kbase_reg_write(kbdev, JOB_CONTROL_REG(JOB_IRQ_THROTTLE),
						irq_throttle_cycles, NULL);
	}

	spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);

	while (done) {
		u32 failed = done >> 16;

		/* treat failed slots as finished slots */
		u32 finished = (done & 0xFFFF) | failed;

		/* Note: This is inherently unfair, as we always check
		 * for lower numbered interrupts before the higher
		 * numbered ones.*/
		i = ffs(finished) - 1;
		KBASE_DEBUG_ASSERT(i >= 0);

		do {
			int nr_done;
			u32 active;
			u32 completion_code = BASE_JD_EVENT_DONE;/* assume OK */
			u64 job_tail = 0;

			if (failed & (1u << i)) {
				/* read out the job slot status code if the job
				 * slot reported failure */
				completion_code = kbase_reg_read(kbdev,
					JOB_SLOT_REG(i, JS_STATUS), NULL);

				switch (completion_code) {
				case BASE_JD_EVENT_STOPPED:
#if defined(CONFIG_MALI_GATOR_SUPPORT)
					kbase_trace_mali_job_slots_event(
						GATOR_MAKE_EVENT(
						GATOR_JOB_SLOT_SOFT_STOPPED, i),
								NULL, 0);
#endif

					kbase_tlstream_aux_job_softstop(i);

					kbasep_trace_tl_nret_atom_lpu(
						kbdev, i);

					/* Soft-stopped job - read the value of
					 * JS<n>_TAIL so that the job chain can
					 * be resumed */
					job_tail = (u64)kbase_reg_read(kbdev,
						JOB_SLOT_REG(i, JS_TAIL_LO),
									NULL) |
						((u64)kbase_reg_read(kbdev,
						JOB_SLOT_REG(i, JS_TAIL_HI),
								NULL) << 32);
					break;
				case BASE_JD_EVENT_NOT_STARTED:
					/* PRLAM-10673 can cause a TERMINATED
					 * job to come back as NOT_STARTED, but
					 * the error interrupt helps us detect
					 * it */
					completion_code =
						BASE_JD_EVENT_TERMINATED;
					/* fall through */
				default:
					dev_warn(kbdev->dev, "error detected from slot %d, job status 0x%08x (%s)",
							i, completion_code,
							kbase_exception_name
							(kbdev,
							completion_code));
				}

				kbase_gpu_irq_evict(kbdev, i);
			}

			kbase_reg_write(kbdev, JOB_CONTROL_REG(JOB_IRQ_CLEAR),
					done & ((1 << i) | (1 << (i + 16))),
					NULL);
			active = kbase_reg_read(kbdev,
					JOB_CONTROL_REG(JOB_IRQ_JS_STATE),
					NULL);

			if (((active >> i) & 1) == 0 &&
					(((done >> (i + 16)) & 1) == 0)) {
				/* There is a potential race we must work
				 * around:
				 *
				 *  1. A job slot has a job in both current and
				 *     next registers
				 *  2. The job in current completes
				 *     successfully, the IRQ handler reads
				 *     RAWSTAT and calls this function with the
				 *     relevant bit set in "done"
				 *  3. The job in the next registers becomes the
				 *     current job on the GPU
				 *  4. Sometime before the JOB_IRQ_CLEAR line
				 *     above the job on the GPU _fails_
				 *  5. The IRQ_CLEAR clears the done bit but not
				 *     the failed bit. This atomically sets
				 *     JOB_IRQ_JS_STATE. However since both jobs
				 *     have now completed the relevant bits for
				 *     the slot are set to 0.
				 *
				 * If we now did nothing then we'd incorrectly
				 * assume that _both_ jobs had completed
				 * successfully (since we haven't yet observed
				 * the fail bit being set in RAWSTAT).
				 *
				 * So at this point if there are no active jobs
				 * left we check to see if RAWSTAT has a failure
				 * bit set for the job slot. If it does we know
				 * that there has been a new failure that we
				 * didn't previously know about, so we make sure
				 * that we record this in active (but we wait
				 * for the next loop to deal with it).
				 *
				 * If we were handling a job failure (i.e. done
				 * has the relevant high bit set) then we know
				 * that the value read back from
				 * JOB_IRQ_JS_STATE is the correct number of
				 * remaining jobs because the failed job will
				 * have prevented any futher jobs from starting
				 * execution.
				 */
				u32 rawstat = kbase_reg_read(kbdev,
					JOB_CONTROL_REG(JOB_IRQ_RAWSTAT), NULL);

				if ((rawstat >> (i + 16)) & 1) {
					/* There is a failed job that we've
					 * missed - add it back to active */
					active |= (1u << i);
				}
			}

			dev_dbg(kbdev->dev, "Job ended with status 0x%08X\n",
							completion_code);

			nr_done = kbase_backend_nr_atoms_submitted(kbdev, i);
			nr_done -= (active >> i) & 1;
			nr_done -= (active >> (i + 16)) & 1;

			if (nr_done <= 0) {
				dev_warn(kbdev->dev, "Spurious interrupt on slot %d",
									i);

				goto spurious;
			}

			count += nr_done;

			while (nr_done) {
				if (nr_done == 1) {
					kbase_gpu_complete_hw(kbdev, i,
								completion_code,
								job_tail,
								&end_timestamp);
					kbase_jm_try_kick_all(kbdev);
				} else {
					/* More than one job has completed.
					 * Since this is not the last job being
					 * reported this time it must have
					 * passed. This is because the hardware
					 * will not allow further jobs in a job
					 * slot to complete until the failed job
					 * is cleared from the IRQ status.
					 */
					kbase_gpu_complete_hw(kbdev, i,
							BASE_JD_EVENT_DONE,
							0,
							&end_timestamp);
				}
				nr_done--;
			}
 spurious:
			done = kbase_reg_read(kbdev,
					JOB_CONTROL_REG(JOB_IRQ_RAWSTAT), NULL);

			if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_10883)) {
				/* Workaround for missing interrupt caused by
				 * PRLAM-10883 */
				if (((active >> i) & 1) && (0 ==
						kbase_reg_read(kbdev,
							JOB_SLOT_REG(i,
							JS_STATUS), NULL))) {
					/* Force job slot to be processed again
					 */
					done |= (1u << i);
				}
			}

			failed = done >> 16;
			finished = (done & 0xFFFF) | failed;
			if (done)
				end_timestamp = ktime_get();
		} while (finished & (1 << i));

		kbasep_job_slot_update_head_start_timestamp(kbdev, i,
								end_timestamp);
	}

	spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);
#if KBASE_GPU_RESET_EN
	if (atomic_read(&kbdev->hwaccess.backend.reset_gpu) ==
						KBASE_RESET_GPU_COMMITTED) {
		/* If we're trying to reset the GPU then we might be able to do
		 * it early (without waiting for a timeout) because some jobs
		 * have completed
		 */
		kbasep_try_reset_gpu_early(kbdev);
	}
#endif /* KBASE_GPU_RESET_EN */
	KBASE_TRACE_ADD(kbdev, JM_IRQ_END, NULL, NULL, 0, count);
}
KBASE_EXPORT_TEST_API(kbase_job_done);

static bool kbasep_soft_stop_allowed(struct kbase_device *kbdev,
					struct kbase_jd_atom *katom)
{
	bool soft_stops_allowed = true;

	if (kbase_jd_katom_is_secure(katom)) {
		soft_stops_allowed = false;
	} else if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_8408)) {
		if ((katom->core_req & BASE_JD_REQ_T) != 0)
			soft_stops_allowed = false;
	}
	return soft_stops_allowed;
}

static bool kbasep_hard_stop_allowed(struct kbase_device *kbdev,
								u16 core_reqs)
{
	bool hard_stops_allowed = true;

	if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_8394)) {
		if ((core_reqs & BASE_JD_REQ_T) != 0)
			hard_stops_allowed = false;
	}
	return hard_stops_allowed;
}

void kbasep_job_slot_soft_or_hard_stop_do_action(struct kbase_device *kbdev,
					int js,
					u32 action,
					u16 core_reqs,
					struct kbase_jd_atom *target_katom)
{
	struct kbase_context *kctx = target_katom->kctx;
#if KBASE_TRACE_ENABLE
	u32 status_reg_before;
	u64 job_in_head_before;
	u32 status_reg_after;

	KBASE_DEBUG_ASSERT(!(action & (~JS_COMMAND_MASK)));

	/* Check the head pointer */
	job_in_head_before = ((u64) kbase_reg_read(kbdev,
					JOB_SLOT_REG(js, JS_HEAD_LO), NULL))
			| (((u64) kbase_reg_read(kbdev,
					JOB_SLOT_REG(js, JS_HEAD_HI), NULL))
									<< 32);
	status_reg_before = kbase_reg_read(kbdev, JOB_SLOT_REG(js, JS_STATUS),
									NULL);
#endif

	if (action == JS_COMMAND_SOFT_STOP) {
		bool soft_stop_allowed = kbasep_soft_stop_allowed(kbdev,
								target_katom);

		if (!soft_stop_allowed) {
#ifdef CONFIG_MALI_DEBUG
			dev_dbg(kbdev->dev,
					"Attempt made to soft-stop a job that cannot be soft-stopped. core_reqs = 0x%X",
					(unsigned int)core_reqs);
#endif				/* CONFIG_MALI_DEBUG */
			return;
		}

		/* We are about to issue a soft stop, so mark the atom as having
		 * been soft stopped */
		target_katom->atom_flags |= KBASE_KATOM_FLAG_BEEN_SOFT_STOPPPED;

		/* Mark the point where we issue the soft-stop command */
		kbase_tlstream_aux_issue_job_softstop(target_katom);

		if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_8316)) {
			int i;

			for (i = 0;
			     i < kbase_backend_nr_atoms_submitted(kbdev, js);
			     i++) {
				struct kbase_jd_atom *katom;

				katom = kbase_gpu_inspect(kbdev, js, i);

				KBASE_DEBUG_ASSERT(katom);

				/* For HW_ISSUE_8316, only 'bad' jobs attacking
				 * the system can cause this issue: normally,
				 * all memory should be allocated in multiples
				 * of 4 pages, and growable memory should be
				 * changed size in multiples of 4 pages.
				 *
				 * Whilst such 'bad' jobs can be cleared by a
				 * GPU reset, the locking up of a uTLB entry
				 * caused by the bad job could also stall other
				 * ASs, meaning that other ASs' jobs don't
				 * complete in the 'grace' period before the
				 * reset. We don't want to lose other ASs' jobs
				 * when they would normally complete fine, so we
				 * must 'poke' the MMU regularly to help other
				 * ASs complete */
				kbase_as_poking_timer_retain_atom(
						kbdev, katom->kctx, katom);
			}
		}

		if (kbase_hw_has_feature(
				kbdev,
				BASE_HW_FEATURE_JOBCHAIN_DISAMBIGUATION)) {
			action = (target_katom->atom_flags &
					KBASE_KATOM_FLAGS_JOBCHAIN) ?
				JS_COMMAND_SOFT_STOP_1 :
				JS_COMMAND_SOFT_STOP_0;
		}
	} else if (action == JS_COMMAND_HARD_STOP) {
		bool hard_stop_allowed = kbasep_hard_stop_allowed(kbdev,
								core_reqs);

		if (!hard_stop_allowed) {
			/* Jobs can be hard-stopped for the following reasons:
			 *  * CFS decides the job has been running too long (and
			 *    soft-stop has not occurred). In this case the GPU
			 *    will be reset by CFS if the job remains on the
			 *    GPU.
			 *
			 *  * The context is destroyed, kbase_jd_zap_context
			 *    will attempt to hard-stop the job. However it also
			 *    has a watchdog which will cause the GPU to be
			 *    reset if the job remains on the GPU.
			 *
			 *  * An (unhandled) MMU fault occurred. As long as
			 *    BASE_HW_ISSUE_8245 is defined then the GPU will be
			 *    reset.
			 *
			 * All three cases result in the GPU being reset if the
			 * hard-stop fails, so it is safe to just return and
			 * ignore the hard-stop request.
			 */
			dev_warn(kbdev->dev,
					"Attempt made to hard-stop a job that cannot be hard-stopped. core_reqs = 0x%X",
					(unsigned int)core_reqs);
			return;
		}
#ifdef CONFIG_DEBUG_FS
		/*
		 * If job fault dumping is enabled then don't actually hard stop
		 * to give the fault dumping mechanism time to capture the dump
		 */
		if (kbase_debug_job_fault_process(target_katom,
					BASE_JD_EVENT_JOB_CANCELLED))
			return;
#endif

		target_katom->atom_flags |= KBASE_KATOM_FLAG_BEEN_HARD_STOPPED;

		if (kbase_hw_has_feature(
				kbdev,
				BASE_HW_FEATURE_JOBCHAIN_DISAMBIGUATION)) {
			action = (target_katom->atom_flags &
					KBASE_KATOM_FLAGS_JOBCHAIN) ?
				JS_COMMAND_HARD_STOP_1 :
				JS_COMMAND_HARD_STOP_0;
		}
	}

	kbase_reg_write(kbdev, JOB_SLOT_REG(js, JS_COMMAND), action, kctx);

#if KBASE_TRACE_ENABLE
	status_reg_after = kbase_reg_read(kbdev, JOB_SLOT_REG(js, JS_STATUS),
									NULL);
	if (status_reg_after == BASE_JD_EVENT_ACTIVE) {
		struct kbase_jd_atom *head;
		struct kbase_context *head_kctx;

		head = kbase_gpu_inspect(kbdev, js, 0);
		head_kctx = head->kctx;

		if (status_reg_before == BASE_JD_EVENT_ACTIVE)
			KBASE_TRACE_ADD_SLOT(kbdev, JM_CHECK_HEAD, head_kctx,
						head, job_in_head_before, js);
		else
			KBASE_TRACE_ADD_SLOT(kbdev, JM_CHECK_HEAD, NULL, NULL,
						0, js);

		switch (action) {
		case JS_COMMAND_SOFT_STOP:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP, head_kctx,
							head, head->jc, js);
			break;
		case JS_COMMAND_SOFT_STOP_0:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP_0, head_kctx,
							head, head->jc, js);
			break;
		case JS_COMMAND_SOFT_STOP_1:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP_1, head_kctx,
							head, head->jc, js);
			break;
		case JS_COMMAND_HARD_STOP:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP, head_kctx,
							head, head->jc, js);
			break;
		case JS_COMMAND_HARD_STOP_0:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP_0, head_kctx,
							head, head->jc, js);
			break;
		case JS_COMMAND_HARD_STOP_1:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP_1, head_kctx,
							head, head->jc, js);
			break;
		default:
			BUG();
			break;
		}
	} else {
		if (status_reg_before == BASE_JD_EVENT_ACTIVE)
			KBASE_TRACE_ADD_SLOT(kbdev, JM_CHECK_HEAD, NULL, NULL,
							job_in_head_before, js);
		else
			KBASE_TRACE_ADD_SLOT(kbdev, JM_CHECK_HEAD, NULL, NULL,
							0, js);

		switch (action) {
		case JS_COMMAND_SOFT_STOP:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP, NULL, NULL, 0,
							js);
			break;
		case JS_COMMAND_SOFT_STOP_0:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP_0, NULL, NULL,
							0, js);
			break;
		case JS_COMMAND_SOFT_STOP_1:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_SOFTSTOP_1, NULL, NULL,
							0, js);
			break;
		case JS_COMMAND_HARD_STOP:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP, NULL, NULL, 0,
							js);
			break;
		case JS_COMMAND_HARD_STOP_0:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP_0, NULL, NULL,
							0, js);
			break;
		case JS_COMMAND_HARD_STOP_1:
			KBASE_TRACE_ADD_SLOT(kbdev, JM_HARDSTOP_1, NULL, NULL,
							0, js);
			break;
		default:
			BUG();
			break;
		}
	}
#endif
}

void kbase_backend_jm_kill_jobs_from_kctx(struct kbase_context *kctx)
{
	unsigned long flags;
	struct kbase_device *kbdev;
	struct kbasep_js_device_data *js_devdata;
	int i;

	KBASE_DEBUG_ASSERT(kctx != NULL);
	kbdev = kctx->kbdev;
	KBASE_DEBUG_ASSERT(kbdev != NULL);
	js_devdata = &kbdev->js_data;

	/* Cancel any remaining running jobs for this kctx  */
	mutex_lock(&kctx->jctx.lock);
	spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);

	/* Invalidate all jobs in context, to prevent re-submitting */
	for (i = 0; i < BASE_JD_ATOM_COUNT; i++) {
		if (!work_pending(&kctx->jctx.atoms[i].work))
			kctx->jctx.atoms[i].event_code =
						BASE_JD_EVENT_JOB_CANCELLED;
	}

	for (i = 0; i < kbdev->gpu_props.num_job_slots; i++)
		kbase_job_slot_hardstop(kctx, i, NULL);

	spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);
	mutex_unlock(&kctx->jctx.lock);
}

void kbase_job_slot_ctx_priority_check_locked(struct kbase_context *kctx,
				struct kbase_jd_atom *target_katom)
{
	struct kbase_device *kbdev;
	struct kbasep_js_device_data *js_devdata;
	int js = target_katom->slot_nr;
	int priority = target_katom->sched_priority;
	int i;

	KBASE_DEBUG_ASSERT(kctx != NULL);
	kbdev = kctx->kbdev;
	KBASE_DEBUG_ASSERT(kbdev != NULL);
	js_devdata = &kbdev->js_data;

	lockdep_assert_held(&kbdev->js_data.runpool_irq.lock);

	for (i = 0; i < kbase_backend_nr_atoms_on_slot(kbdev, js); i++) {
		struct kbase_jd_atom *katom;

		katom = kbase_gpu_inspect(kbdev, js, i);
		if (!katom)
			continue;

		if (katom->kctx != kctx)
			continue;

		if (katom->sched_priority > priority)
			kbase_job_slot_softstop(kbdev, js, katom);
	}
}

struct zap_reset_data {
	/* The stages are:
	 * 1. The timer has never been called
	 * 2. The zap has timed out, all slots are soft-stopped - the GPU reset
	 *    will happen. The GPU has been reset when
	 *    kbdev->hwaccess.backend.reset_waitq is signalled
	 *
	 * (-1 - The timer has been cancelled)
	 */
	int stage;
	struct kbase_device *kbdev;
	struct hrtimer timer;
	spinlock_t lock; /* protects updates to stage member */
};

static enum hrtimer_restart zap_timeout_callback(struct hrtimer *timer)
{
	struct zap_reset_data *reset_data = container_of(timer,
						struct zap_reset_data, timer);
	struct kbase_device *kbdev = reset_data->kbdev;
	unsigned long flags;

	spin_lock_irqsave(&reset_data->lock, flags);

	if (reset_data->stage == -1)
		goto out;

#if KBASE_GPU_RESET_EN
	if (kbase_prepare_to_reset_gpu(kbdev)) {
		dev_err(kbdev->dev, "Issueing GPU soft-reset because jobs failed to be killed (within %d ms) as part of context termination (e.g. process exit)\n",
								ZAP_TIMEOUT);
		kbase_reset_gpu(kbdev);
	}
#endif /* KBASE_GPU_RESET_EN */
	reset_data->stage = 2;

 out:
	spin_unlock_irqrestore(&reset_data->lock, flags);

	return HRTIMER_NORESTART;
}

void kbase_jm_wait_for_zero_jobs(struct kbase_context *kctx)
{
	struct kbase_device *kbdev = kctx->kbdev;
	struct zap_reset_data reset_data;
	unsigned long flags;

	hrtimer_init_on_stack(&reset_data.timer, CLOCK_MONOTONIC,
							HRTIMER_MODE_REL);
	reset_data.timer.function = zap_timeout_callback;

	spin_lock_init(&reset_data.lock);

	reset_data.kbdev = kbdev;
	reset_data.stage = 1;

	hrtimer_start(&reset_data.timer, HR_TIMER_DELAY_MSEC(ZAP_TIMEOUT),
							HRTIMER_MODE_REL);

	/* Wait for all jobs to finish, and for the context to be not-scheduled
	 * (due to kbase_job_zap_context(), we also guarentee it's not in the JS
	 * policy queue either */
	wait_event(kctx->jctx.zero_jobs_wait, kctx->jctx.job_nr == 0);
	wait_event(kctx->jctx.sched_info.ctx.is_scheduled_wait,
			kctx->jctx.sched_info.ctx.is_scheduled == false);

	spin_lock_irqsave(&reset_data.lock, flags);
	if (reset_data.stage == 1) {
		/* The timer hasn't run yet - so cancel it */
		reset_data.stage = -1;
	}
	spin_unlock_irqrestore(&reset_data.lock, flags);

	hrtimer_cancel(&reset_data.timer);

	if (reset_data.stage == 2) {
		/* The reset has already started.
		 * Wait for the reset to complete
		 */
		wait_event(kbdev->hwaccess.backend.reset_wait,
				atomic_read(&kbdev->hwaccess.backend.reset_gpu)
						== KBASE_RESET_GPU_NOT_PENDING);
	}
	destroy_hrtimer_on_stack(&reset_data.timer);

	dev_dbg(kbdev->dev, "Zap: Finished Context %p", kctx);

	/* Ensure that the signallers of the waitqs have finished */
	mutex_lock(&kctx->jctx.lock);
	mutex_lock(&kctx->jctx.sched_info.ctx.jsctx_mutex);
	mutex_unlock(&kctx->jctx.sched_info.ctx.jsctx_mutex);
	mutex_unlock(&kctx->jctx.lock);
}

u32 kbase_backend_get_current_flush_id(struct kbase_device *kbdev)
{
	u32 flush_id = 0;

	if (kbase_hw_has_feature(kbdev, BASE_HW_FEATURE_FLUSH_REDUCTION)) {
		mutex_lock(&kbdev->pm.lock);
		if (kbdev->pm.backend.gpu_powered)
			flush_id = kbase_reg_read(kbdev,
					GPU_CONTROL_REG(LATEST_FLUSH), NULL);
		mutex_unlock(&kbdev->pm.lock);
	}

	return flush_id;
}

int kbase_job_slot_init(struct kbase_device *kbdev)
{
#if KBASE_GPU_RESET_EN
	kbdev->hwaccess.backend.reset_workq = alloc_workqueue(
						"Mali reset workqueue", 0, 1);
	if (NULL == kbdev->hwaccess.backend.reset_workq)
		return -EINVAL;

	KBASE_DEBUG_ASSERT(0 ==
		object_is_on_stack(&kbdev->hwaccess.backend.reset_work));
	INIT_WORK(&kbdev->hwaccess.backend.reset_work,
						kbasep_reset_timeout_worker);

	hrtimer_init(&kbdev->hwaccess.backend.reset_timer, CLOCK_MONOTONIC,
							HRTIMER_MODE_REL);
	kbdev->hwaccess.backend.reset_timer.function =
						kbasep_reset_timer_callback;
#endif

	return 0;
}
KBASE_EXPORT_TEST_API(kbase_job_slot_init);

void kbase_job_slot_halt(struct kbase_device *kbdev)
{
	CSTD_UNUSED(kbdev);
}

void kbase_job_slot_term(struct kbase_device *kbdev)
{
#if KBASE_GPU_RESET_EN
	destroy_workqueue(kbdev->hwaccess.backend.reset_workq);
#endif
}
KBASE_EXPORT_TEST_API(kbase_job_slot_term);

#if KBASE_GPU_RESET_EN
/**
 * kbasep_check_for_afbc_on_slot() - Check whether AFBC is in use on this slot
 * @kbdev: kbase device pointer
 * @kctx:  context to check against
 * @js:	   slot to check
 * @target_katom: An atom to check, or NULL if all atoms from @kctx on
 *                slot @js should be checked
 *
 * This checks are based upon parameters that would normally be passed to
 * kbase_job_slot_hardstop().
 *
 * In the event of @target_katom being NULL, this will check the last jobs that
 * are likely to be running on the slot to see if a) they belong to kctx, and
 * so would be stopped, and b) whether they have AFBC
 *
 * In that case, It's guaranteed that a job currently executing on the HW with
 * AFBC will be detected. However, this is a conservative check because it also
 * detects jobs that have just completed too.
 *
 * Return: true when hard-stop _might_ stop an afbc atom, else false.
 */
static bool kbasep_check_for_afbc_on_slot(struct kbase_device *kbdev,
		struct kbase_context *kctx, int js,
		struct kbase_jd_atom *target_katom)
{
	bool ret = false;
	int i;

	lockdep_assert_held(&kbdev->js_data.runpool_irq.lock);

	/* When we have an atom the decision can be made straight away. */
	if (target_katom)
		return !!(target_katom->core_req & BASE_JD_REQ_FS_AFBC);

	/* Otherwise, we must chweck the hardware to see if it has atoms from
	 * this context with AFBC. */
	for (i = 0; i < kbase_backend_nr_atoms_on_slot(kbdev, js); i++) {
		struct kbase_jd_atom *katom;

		katom = kbase_gpu_inspect(kbdev, js, i);
		if (!katom)
			continue;

		/* Ignore atoms from other contexts, they won't be stopped when
		 * we use this for checking if we should hard-stop them */
		if (katom->kctx != kctx)
			continue;

		/* An atom on this slot and this context: check for AFBC */
		if (katom->core_req & BASE_JD_REQ_FS_AFBC) {
			ret = true;
			break;
		}
	}

	return ret;
}
#endif /* KBASE_GPU_RESET_EN */

/**
 * kbase_job_slot_softstop_swflags - Soft-stop a job with flags
 * @kbdev:         The kbase device
 * @js:            The job slot to soft-stop
 * @target_katom:  The job that should be soft-stopped (or NULL for any job)
 * @sw_flags:      Flags to pass in about the soft-stop
 *
 * Context:
 *   The job slot lock must be held when calling this function.
 *   The job slot must not already be in the process of being soft-stopped.
 *
 * Soft-stop the specified job slot, with extra information about the stop
 *
 * Where possible any job in the next register is evicted before the soft-stop.
 */
void kbase_job_slot_softstop_swflags(struct kbase_device *kbdev, int js,
			struct kbase_jd_atom *target_katom, u32 sw_flags)
{
	KBASE_DEBUG_ASSERT(!(sw_flags & JS_COMMAND_MASK));
	kbase_backend_soft_hard_stop_slot(kbdev, NULL, js, target_katom,
			JS_COMMAND_SOFT_STOP | sw_flags);
}

/**
 * kbase_job_slot_softstop - Soft-stop the specified job slot
 * @kbdev:         The kbase device
 * @js:            The job slot to soft-stop
 * @target_katom:  The job that should be soft-stopped (or NULL for any job)
 * Context:
 *   The job slot lock must be held when calling this function.
 *   The job slot must not already be in the process of being soft-stopped.
 *
 * Where possible any job in the next register is evicted before the soft-stop.
 */
void kbase_job_slot_softstop(struct kbase_device *kbdev, int js,
				struct kbase_jd_atom *target_katom)
{
	kbase_job_slot_softstop_swflags(kbdev, js, target_katom, 0u);
}

/**
 * kbase_job_slot_hardstop - Hard-stop the specified job slot
 * @kctx:         The kbase context that contains the job(s) that should
 *                be hard-stopped
 * @js:           The job slot to hard-stop
 * @target_katom: The job that should be hard-stopped (or NULL for all
 *                jobs from the context)
 * Context:
 *   The job slot lock must be held when calling this function.
 */
void kbase_job_slot_hardstop(struct kbase_context *kctx, int js,
				struct kbase_jd_atom *target_katom)
{
	struct kbase_device *kbdev = kctx->kbdev;
	bool stopped;
#if KBASE_GPU_RESET_EN
	/* We make the check for AFBC before evicting/stopping atoms.  Note
	 * that no other thread can modify the slots whilst we have the
	 * runpool_irq lock. */
	int needs_workaround_for_afbc =
			kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_T76X_3542)
			&& kbasep_check_for_afbc_on_slot(kbdev, kctx, js,
					 target_katom);
#endif

	stopped = kbase_backend_soft_hard_stop_slot(kbdev, kctx, js,
							target_katom,
							JS_COMMAND_HARD_STOP);
#if KBASE_GPU_RESET_EN
	if (stopped && (kbase_hw_has_issue(kctx->kbdev, BASE_HW_ISSUE_8401) ||
			kbase_hw_has_issue(kctx->kbdev, BASE_HW_ISSUE_9510) ||
			needs_workaround_for_afbc)) {
		/* MIDBASE-2916 if a fragment job with AFBC encoding is
		 * hardstopped, ensure to do a soft reset also in order to
		 * clear the GPU status.
		 * Workaround for HW issue 8401 has an issue,so after
		 * hard-stopping just reset the GPU. This will ensure that the
		 * jobs leave the GPU.*/
		if (kbase_prepare_to_reset_gpu_locked(kbdev)) {
			dev_err(kbdev->dev, "Issueing GPU soft-reset after hard stopping due to hardware issue");
			kbase_reset_gpu_locked(kbdev);
		}
	}
#endif
}

/**
 * kbase_job_check_enter_disjoint - potentiall enter disjoint mode
 * @kbdev: kbase device
 * @action: the event which has occurred
 * @core_reqs: core requirements of the atom
 * @target_katom: the atom which is being affected
 *
 * For a certain soft/hard-stop action, work out whether to enter disjoint
 * state.
 *
 * This does not register multiple disjoint events if the atom has already
 * started a disjoint period
 *
 * @core_reqs can be supplied as 0 if the atom had not started on the hardware
 * (and so a 'real' soft/hard-stop was not required, but it still interrupted
 * flow, perhaps on another context)
 *
 * kbase_job_check_leave_disjoint() should be used to end the disjoint
 * state when the soft/hard-stop action is complete
 */
void kbase_job_check_enter_disjoint(struct kbase_device *kbdev, u32 action,
		u16 core_reqs, struct kbase_jd_atom *target_katom)
{
	u32 hw_action = action & JS_COMMAND_MASK;

	/* For hard-stop, don't enter if hard-stop not allowed */
	if (hw_action == JS_COMMAND_HARD_STOP &&
			!kbasep_hard_stop_allowed(kbdev, core_reqs))
		return;

	/* For soft-stop, don't enter if soft-stop not allowed, or isn't
	 * causing disjoint */
	if (hw_action == JS_COMMAND_SOFT_STOP &&
			!(kbasep_soft_stop_allowed(kbdev, target_katom) &&
			  (action & JS_COMMAND_SW_CAUSES_DISJOINT)))
		return;

	/* Nothing to do if already logged disjoint state on this atom */
	if (target_katom->atom_flags & KBASE_KATOM_FLAG_IN_DISJOINT)
		return;

	target_katom->atom_flags |= KBASE_KATOM_FLAG_IN_DISJOINT;
	kbase_disjoint_state_up(kbdev);
}

/**
 * kbase_job_check_enter_disjoint - potentially leave disjoint state
 * @kbdev: kbase device
 * @target_katom: atom which is finishing
 *
 * Work out whether to leave disjoint state when finishing an atom that was
 * originated by kbase_job_check_enter_disjoint().
 */
void kbase_job_check_leave_disjoint(struct kbase_device *kbdev,
		struct kbase_jd_atom *target_katom)
{
	if (target_katom->atom_flags & KBASE_KATOM_FLAG_IN_DISJOINT) {
		target_katom->atom_flags &= ~KBASE_KATOM_FLAG_IN_DISJOINT;
		kbase_disjoint_state_down(kbdev);
	}
}


#if KBASE_GPU_RESET_EN
static void kbase_debug_dump_registers(struct kbase_device *kbdev)
{
	int i;

	dev_err(kbdev->dev, "Register state:");
	dev_err(kbdev->dev, "  GPU_IRQ_RAWSTAT=0x%08x GPU_STATUS=0x%08x",
		kbase_reg_read(kbdev, GPU_CONTROL_REG(GPU_IRQ_RAWSTAT), NULL),
		kbase_reg_read(kbdev, GPU_CONTROL_REG(GPU_STATUS), NULL));
	dev_err(kbdev->dev, "  JOB_IRQ_RAWSTAT=0x%08x JOB_IRQ_JS_STATE=0x%08x JOB_IRQ_THROTTLE=0x%08x",
		kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_RAWSTAT), NULL),
		kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_JS_STATE), NULL),
		kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_THROTTLE), NULL));
	for (i = 0; i < 3; i++) {
		dev_err(kbdev->dev, "  JS%d_STATUS=0x%08x      JS%d_HEAD_LO=0x%08x",
			i, kbase_reg_read(kbdev, JOB_SLOT_REG(i, JS_STATUS),
					NULL),
			i, kbase_reg_read(kbdev, JOB_SLOT_REG(i, JS_HEAD_LO),
					NULL));
	}
	dev_err(kbdev->dev, "  MMU_IRQ_RAWSTAT=0x%08x GPU_FAULTSTATUS=0x%08x",
		kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_RAWSTAT), NULL),
		kbase_reg_read(kbdev, GPU_CONTROL_REG(GPU_FAULTSTATUS), NULL));
	dev_err(kbdev->dev, "  GPU_IRQ_MASK=0x%08x    JOB_IRQ_MASK=0x%08x     MMU_IRQ_MASK=0x%08x",
		kbase_reg_read(kbdev, GPU_CONTROL_REG(GPU_IRQ_MASK), NULL),
		kbase_reg_read(kbdev, JOB_CONTROL_REG(JOB_IRQ_MASK), NULL),
		kbase_reg_read(kbdev, MMU_REG(MMU_IRQ_MASK), NULL));
	dev_err(kbdev->dev, "  PWR_OVERRIDE0=0x%08x   PWR_OVERRIDE1=0x%08x",
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PWR_OVERRIDE0), NULL),
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PWR_OVERRIDE1), NULL));
	dev_err(kbdev->dev, "  SHADER_CONFIG=0x%08x   L2_MMU_CONFIG=0x%08x",
		kbase_reg_read(kbdev, GPU_CONTROL_REG(SHADER_CONFIG), NULL),
		kbase_reg_read(kbdev, GPU_CONTROL_REG(L2_MMU_CONFIG), NULL));
}

static void kbasep_save_hwcnt_setup(struct kbase_device *kbdev,
				struct kbase_context *kctx,
				struct kbase_uk_hwcnt_setup *hwcnt_setup)
{
	hwcnt_setup->dump_buffer =
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_BASE_LO), kctx) &
								0xffffffff;
	hwcnt_setup->dump_buffer |= (u64)
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_BASE_HI), kctx) <<
									32;
	hwcnt_setup->jm_bm =
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_JM_EN), kctx);
	hwcnt_setup->shader_bm =
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_SHADER_EN), kctx);
	hwcnt_setup->tiler_bm =
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_TILER_EN), kctx);
	hwcnt_setup->mmu_l2_bm =
		kbase_reg_read(kbdev, GPU_CONTROL_REG(PRFCNT_MMU_L2_EN), kctx);
}

static void kbasep_reset_timeout_worker(struct work_struct *data)
{
	unsigned long flags, mmu_flags;
	struct kbase_device *kbdev;
	int i;
	ktime_t end_timestamp = ktime_get();
	struct kbasep_js_device_data *js_devdata;
	struct kbase_uk_hwcnt_setup hwcnt_setup = { {0} };
	enum kbase_instr_state bckp_state;
	bool try_schedule = false;
	bool restore_hwc = false;

	KBASE_DEBUG_ASSERT(data);

	kbdev = container_of(data, struct kbase_device,
						hwaccess.backend.reset_work);

	KBASE_DEBUG_ASSERT(kbdev);
	js_devdata = &kbdev->js_data;

	KBASE_TRACE_ADD(kbdev, JM_BEGIN_RESET_WORKER, NULL, NULL, 0u, 0);

	/* Make sure the timer has completed - this cannot be done from
	 * interrupt context, so this cannot be done within
	 * kbasep_try_reset_gpu_early. */
	hrtimer_cancel(&kbdev->hwaccess.backend.reset_timer);

	if (kbase_pm_context_active_handle_suspend(kbdev,
				KBASE_PM_SUSPEND_HANDLER_DONT_REACTIVATE)) {
		/* This would re-activate the GPU. Since it's already idle,
		 * there's no need to reset it */
		atomic_set(&kbdev->hwaccess.backend.reset_gpu,
						KBASE_RESET_GPU_NOT_PENDING);
		kbase_disjoint_state_down(kbdev);
		wake_up(&kbdev->hwaccess.backend.reset_wait);
		return;
	}

	KBASE_DEBUG_ASSERT(kbdev->irq_reset_flush == false);

	spin_lock_irqsave(&kbdev->mmu_mask_change, mmu_flags);
	/* We're about to flush out the IRQs and their bottom half's */
	kbdev->irq_reset_flush = true;

	/* Disable IRQ to avoid IRQ handlers to kick in after releasing the
	 * spinlock; this also clears any outstanding interrupts */
	spin_lock_irqsave(&kbdev->hwcnt.lock, flags);
	kbase_pm_disable_interrupts(kbdev);
	spin_unlock_irqrestore(&kbdev->hwcnt.lock, flags);

	spin_unlock_irqrestore(&kbdev->mmu_mask_change, mmu_flags);

	/* Ensure that any IRQ handlers have finished
	 * Must be done without any locks IRQ handlers will take */
	kbase_synchronize_irqs(kbdev);

	/* Flush out any in-flight work items */
	kbase_flush_mmu_wqs(kbdev);

	/* The flush has completed so reset the active indicator */
	kbdev->irq_reset_flush = false;

	mutex_lock(&kbdev->pm.lock);
	/* We hold the pm lock, so there ought to be a current policy */
	KBASE_DEBUG_ASSERT(kbdev->pm.backend.pm_current_policy);

	/* All slot have been soft-stopped and we've waited
	 * SOFT_STOP_RESET_TIMEOUT for the slots to clear, at this point we
	 * assume that anything that is still left on the GPU is stuck there and
	 * we'll kill it when we reset the GPU */

	dev_err(kbdev->dev, "Resetting GPU (allowing up to %d ms)",
								RESET_TIMEOUT);

	spin_lock_irqsave(&kbdev->hwcnt.lock, flags);

	if (kbdev->hwcnt.backend.state == KBASE_INSTR_STATE_RESETTING) {
		/* the same interrupt handler preempted itself */
		/* GPU is being reset */
		spin_unlock_irqrestore(&kbdev->hwcnt.lock, flags);
		wait_event(kbdev->hwcnt.backend.wait,
					kbdev->hwcnt.backend.triggered != 0);
		spin_lock_irqsave(&kbdev->hwcnt.lock, flags);
	}
	/* Save the HW counters setup */
	if (kbdev->hwcnt.kctx != NULL) {
		struct kbase_context *kctx = kbdev->hwcnt.kctx;

		if (kctx->jctx.sched_info.ctx.is_scheduled) {
			kbasep_save_hwcnt_setup(kbdev, kctx, &hwcnt_setup);

			restore_hwc = true;
		}
	}

	/* Output the state of some interesting registers to help in the
	 * debugging of GPU resets */
	kbase_debug_dump_registers(kbdev);

	bckp_state = kbdev->hwcnt.backend.state;
	kbdev->hwcnt.backend.state = KBASE_INSTR_STATE_RESETTING;
	kbdev->hwcnt.backend.triggered = 0;

	spin_unlock_irqrestore(&kbdev->hwcnt.lock, flags);

	/* Reset the GPU */
	kbase_pm_init_hw(kbdev, 0);

	/* Complete any jobs that were still on the GPU */
	spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);
	kbase_backend_reset(kbdev, &end_timestamp);
	kbase_pm_metrics_update(kbdev, NULL);
	spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);

	mutex_unlock(&kbdev->pm.lock);

	mutex_lock(&js_devdata->runpool_mutex);

	/* Reprogram the GPU's MMU */
	for (i = 0; i < kbdev->nr_hw_address_spaces; i++) {
		struct kbase_as *as = &kbdev->as[i];

		mutex_lock(&as->transaction_mutex);
		spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);

		if (js_devdata->runpool_irq.per_as_data[i].kctx)
			kbase_mmu_update(
				js_devdata->runpool_irq.per_as_data[i].kctx);
		else
			kbase_mmu_disable_as(kbdev, i);

		spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);
		mutex_unlock(&as->transaction_mutex);
	}

	kbase_pm_enable_interrupts(kbdev);

	atomic_set(&kbdev->hwaccess.backend.reset_gpu,
						KBASE_RESET_GPU_NOT_PENDING);

	kbase_disjoint_state_down(kbdev);

	wake_up(&kbdev->hwaccess.backend.reset_wait);
	dev_err(kbdev->dev, "Reset complete");

	if (js_devdata->nr_contexts_pullable > 0 && !kbdev->poweroff_pending)
		try_schedule = true;

	mutex_unlock(&js_devdata->runpool_mutex);

	spin_lock_irqsave(&kbdev->hwcnt.lock, flags);
	/* Restore the HW counters setup */
	if (restore_hwc) {
		struct kbase_context *kctx = kbdev->hwcnt.kctx;
		u32 prfcnt_config = kctx->as_nr << PRFCNT_CONFIG_AS_SHIFT;

#ifdef CONFIG_MALI_PRFCNT_SET_SECONDARY
		u32 gpu_id = kbdev->gpu_props.props.raw_props.gpu_id;
		u32 product_id = (gpu_id & GPU_ID_VERSION_PRODUCT_ID)
			>> GPU_ID_VERSION_PRODUCT_ID_SHIFT;
		int arch_v6 = GPU_ID_IS_NEW_FORMAT(product_id);

		if (arch_v6)
			prfcnt_config |= 1 << PRFCNT_CONFIG_SETSELECT_SHIFT;
#endif

		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_CONFIG),
				prfcnt_config | PRFCNT_CONFIG_MODE_OFF, kctx);

		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_BASE_LO),
				hwcnt_setup.dump_buffer & 0xFFFFFFFF, kctx);
		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_BASE_HI),
				hwcnt_setup.dump_buffer >> 32,        kctx);
		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_JM_EN),
				hwcnt_setup.jm_bm,                    kctx);
		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_SHADER_EN),
				hwcnt_setup.shader_bm,                kctx);
		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_MMU_L2_EN),
				hwcnt_setup.mmu_l2_bm,                kctx);

		/* Due to PRLAM-8186 we need to disable the Tiler before we
		 * enable the HW counter dump. */
		if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_8186))
			kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_TILER_EN),
						0, kctx);
		else
			kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_TILER_EN),
						hwcnt_setup.tiler_bm, kctx);

		kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_CONFIG),
				prfcnt_config | PRFCNT_CONFIG_MODE_MANUAL,
				kctx);

		/* If HW has PRLAM-8186 we can now re-enable the tiler HW
		 * counters dump */
		if (kbase_hw_has_issue(kbdev, BASE_HW_ISSUE_8186))
			kbase_reg_write(kbdev, GPU_CONTROL_REG(PRFCNT_TILER_EN),
						hwcnt_setup.tiler_bm, kctx);
	}
	kbdev->hwcnt.backend.state = bckp_state;
	switch (kbdev->hwcnt.backend.state) {
	/* Cases for waking kbasep_cache_clean_worker worker */
	case KBASE_INSTR_STATE_CLEANED:
		/* Cache-clean IRQ occurred, but we reset:
		 * Wakeup incase the waiter saw RESETTING */
	case KBASE_INSTR_STATE_REQUEST_CLEAN:
		/* After a clean was requested, but before the regs were
		 * written:
		 * Wakeup incase the waiter saw RESETTING */
		wake_up(&kbdev->hwcnt.backend.cache_clean_wait);
		break;
	case KBASE_INSTR_STATE_CLEANING:
		/* Either:
		 * 1) We've not got the Cache-clean IRQ yet: it was lost, or:
		 * 2) We got it whilst resetting: it was voluntarily lost
		 *
		 * So, move to the next state and wakeup: */
		kbdev->hwcnt.backend.state = KBASE_INSTR_STATE_CLEANED;
		wake_up(&kbdev->hwcnt.backend.cache_clean_wait);
		break;

	/* Cases for waking anyone else */
	case KBASE_INSTR_STATE_DUMPING:
		/* If dumping, abort the dump, because we may've lost the IRQ */
		kbdev->hwcnt.backend.state = KBASE_INSTR_STATE_IDLE;
		kbdev->hwcnt.backend.triggered = 1;
		wake_up(&kbdev->hwcnt.backend.wait);
		break;
	case KBASE_INSTR_STATE_DISABLED:
	case KBASE_INSTR_STATE_IDLE:
	case KBASE_INSTR_STATE_FAULT:
		/* Every other reason: wakeup in that state */
		kbdev->hwcnt.backend.triggered = 1;
		wake_up(&kbdev->hwcnt.backend.wait);
		break;

	/* Unhandled cases */
	case KBASE_INSTR_STATE_RESETTING:
	default:
		BUG();
		break;
	}
	spin_unlock_irqrestore(&kbdev->hwcnt.lock, flags);

	/* Resume the vinstr core */
	kbase_vinstr_hwc_resume(kbdev->vinstr_ctx);

	/* Note: counter dumping may now resume */

	mutex_lock(&kbdev->pm.lock);

	/* Find out what cores are required now */
	kbase_pm_update_cores_state(kbdev);

	/* Synchronously request and wait for those cores, because if
	 * instrumentation is enabled it would need them immediately. */
	kbase_pm_check_transitions_sync(kbdev);

	mutex_unlock(&kbdev->pm.lock);

	/* Try submitting some jobs to restart processing */
	if (try_schedule) {
		KBASE_TRACE_ADD(kbdev, JM_SUBMIT_AFTER_RESET, NULL, NULL, 0u,
									0);
		kbase_js_sched_all(kbdev);
	}

	kbase_pm_context_idle(kbdev);
	KBASE_TRACE_ADD(kbdev, JM_END_RESET_WORKER, NULL, NULL, 0u, 0);
}

static enum hrtimer_restart kbasep_reset_timer_callback(struct hrtimer *timer)
{
	struct kbase_device *kbdev = container_of(timer, struct kbase_device,
						hwaccess.backend.reset_timer);

	KBASE_DEBUG_ASSERT(kbdev);

	/* Reset still pending? */
	if (atomic_cmpxchg(&kbdev->hwaccess.backend.reset_gpu,
			KBASE_RESET_GPU_COMMITTED, KBASE_RESET_GPU_HAPPENING) ==
						KBASE_RESET_GPU_COMMITTED)
		queue_work(kbdev->hwaccess.backend.reset_workq,
					&kbdev->hwaccess.backend.reset_work);

	return HRTIMER_NORESTART;
}

/*
 * If all jobs are evicted from the GPU then we can reset the GPU
 * immediately instead of waiting for the timeout to elapse
 */

static void kbasep_try_reset_gpu_early_locked(struct kbase_device *kbdev)
{
	int i;
	int pending_jobs = 0;

	KBASE_DEBUG_ASSERT(kbdev);

	/* Count the number of jobs */
	for (i = 0; i < kbdev->gpu_props.num_job_slots; i++)
		pending_jobs += kbase_backend_nr_atoms_submitted(kbdev, i);

	if (pending_jobs > 0) {
		/* There are still jobs on the GPU - wait */
		return;
	}

	/* To prevent getting incorrect registers when dumping failed job,
	 * skip early reset.
	 */
	if (kbdev->job_fault_debug != false)
		return;

	/* Check that the reset has been committed to (i.e. kbase_reset_gpu has
	 * been called), and that no other thread beat this thread to starting
	 * the reset */
	if (atomic_cmpxchg(&kbdev->hwaccess.backend.reset_gpu,
			KBASE_RESET_GPU_COMMITTED, KBASE_RESET_GPU_HAPPENING) !=
						KBASE_RESET_GPU_COMMITTED) {
		/* Reset has already occurred */
		return;
	}

	queue_work(kbdev->hwaccess.backend.reset_workq,
					&kbdev->hwaccess.backend.reset_work);
}

static void kbasep_try_reset_gpu_early(struct kbase_device *kbdev)
{
	unsigned long flags;
	struct kbasep_js_device_data *js_devdata;

	js_devdata = &kbdev->js_data;
	spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);
	kbasep_try_reset_gpu_early_locked(kbdev);
	spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);
}

/**
 * kbase_prepare_to_reset_gpu_locked - Prepare for resetting the GPU
 * @kbdev: kbase device
 *
 * This function just soft-stops all the slots to ensure that as many jobs as
 * possible are saved.
 *
 * Return:
 *   The function returns a boolean which should be interpreted as follows:
 *   true - Prepared for reset, kbase_reset_gpu should be called.
 *   false - Another thread is performing a reset, kbase_reset_gpu should
 *   not be called.
 */
bool kbase_prepare_to_reset_gpu_locked(struct kbase_device *kbdev)
{
	int i;

	KBASE_DEBUG_ASSERT(kbdev);

	if (atomic_cmpxchg(&kbdev->hwaccess.backend.reset_gpu,
						KBASE_RESET_GPU_NOT_PENDING,
						KBASE_RESET_GPU_PREPARED) !=
						KBASE_RESET_GPU_NOT_PENDING) {
		/* Some other thread is already resetting the GPU */
		return false;
	}

	kbase_disjoint_state_up(kbdev);

	for (i = 0; i < kbdev->gpu_props.num_job_slots; i++)
		kbase_job_slot_softstop(kbdev, i, NULL);

	return true;
}

bool kbase_prepare_to_reset_gpu(struct kbase_device *kbdev)
{
	unsigned long flags;
	bool ret;
	struct kbasep_js_device_data *js_devdata;

	js_devdata = &kbdev->js_data;
	spin_lock_irqsave(&js_devdata->runpool_irq.lock, flags);
	ret = kbase_prepare_to_reset_gpu_locked(kbdev);
	spin_unlock_irqrestore(&js_devdata->runpool_irq.lock, flags);

	return ret;
}
KBASE_EXPORT_TEST_API(kbase_prepare_to_reset_gpu);

/*
 * This function should be called after kbase_prepare_to_reset_gpu if it
 * returns true. It should never be called without a corresponding call to
 * kbase_prepare_to_reset_gpu.
 *
 * After this function is called (or not called if kbase_prepare_to_reset_gpu
 * returned false), the caller should wait for
 * kbdev->hwaccess.backend.reset_waitq to be signalled to know when the reset
 * has completed.
 */
void kbase_reset_gpu(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev);

	/* Note this is an assert/atomic_set because it is a software issue for
	 * a race to be occuring here */
	KBASE_DEBUG_ASSERT(atomic_read(&kbdev->hwaccess.backend.reset_gpu) ==
						KBASE_RESET_GPU_PREPARED);
	atomic_set(&kbdev->hwaccess.backend.reset_gpu,
						KBASE_RESET_GPU_COMMITTED);

	dev_err(kbdev->dev, "Preparing to soft-reset GPU: Waiting (upto %d ms) for all jobs to complete soft-stop\n",
			kbdev->reset_timeout_ms);

	hrtimer_start(&kbdev->hwaccess.backend.reset_timer,
			HR_TIMER_DELAY_MSEC(kbdev->reset_timeout_ms),
			HRTIMER_MODE_REL);

	/* Try resetting early */
	kbasep_try_reset_gpu_early(kbdev);
}
KBASE_EXPORT_TEST_API(kbase_reset_gpu);

void kbase_reset_gpu_locked(struct kbase_device *kbdev)
{
	KBASE_DEBUG_ASSERT(kbdev);

	/* Note this is an assert/atomic_set because it is a software issue for
	 * a race to be occuring here */
	KBASE_DEBUG_ASSERT(atomic_read(&kbdev->hwaccess.backend.reset_gpu) ==
						KBASE_RESET_GPU_PREPARED);
	atomic_set(&kbdev->hwaccess.backend.reset_gpu,
						KBASE_RESET_GPU_COMMITTED);

	dev_err(kbdev->dev, "Preparing to soft-reset GPU: Waiting (upto %d ms) for all jobs to complete soft-stop\n",
			kbdev->reset_timeout_ms);
	hrtimer_start(&kbdev->hwaccess.backend.reset_timer,
			HR_TIMER_DELAY_MSEC(kbdev->reset_timeout_ms),
			HRTIMER_MODE_REL);

	/* Try resetting early */
	kbasep_try_reset_gpu_early_locked(kbdev);
}
#endif /* KBASE_GPU_RESET_EN */

/*
 * Copyright 2018 The Hafnium Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hf/api.h"

#include <assert.h>

#include "hf/arch/cpu.h"
#include "hf/arch/std.h"
#include "hf/arch/timer.h"

#include "hf/dlog.h"
#include "hf/mm.h"
#include "hf/spinlock.h"
#include "hf/vm.h"

#include "vmapi/hf/call.h"

/*
 * To eliminate the risk of deadlocks, we define a partial order for the
 * acquisition of locks held concurrently by the same physical CPU. Our current
 * ordering requirements are as follows:
 *
 * vm::lock -> vcpu::lock
 *
 * Locks of the same kind require the lock of lowest address to be locked first,
 * see `sl_lock_both()`.
 */

static_assert(HF_MAILBOX_SIZE == PAGE_SIZE,
	      "Currently, a page is mapped for the send and receive buffers so "
	      "the maximum request is the size of a page.");

static struct mpool api_page_pool;

/**
 * Initialises the API page pool by taking ownership of the contents of the
 * given page pool.
 */
void api_init(struct mpool *ppool)
{
	mpool_init_from(&api_page_pool, ppool);
}

/**
 * Switches the physical CPU back to the corresponding vcpu of the primary VM.
 *
 * This triggers the scheduling logic to run. Run in the context of secondary VM
 * to cause HF_VCPU_RUN to return and the primary VM to regain control of the
 * cpu.
 */
static struct vcpu *api_switch_to_primary(struct vcpu *current,
					  struct hf_vcpu_run_return primary_ret,
					  enum vcpu_state secondary_state)
{
	struct vm *primary = vm_get(HF_PRIMARY_VM_ID);
	struct vcpu *next = &primary->vcpus[cpu_index(current->cpu)];

	/*
	 * If the secondary is blocked but has a timer running, sleep until the
	 * timer fires rather than indefinitely.
	 */
	if (primary_ret.code == HF_VCPU_RUN_WAIT_FOR_INTERRUPT &&
	    arch_timer_enabled_current()) {
		primary_ret.code = HF_VCPU_RUN_SLEEP;
		primary_ret.sleep.ns = arch_timer_remaining_ns_current();
	}

	/* Set the return value for the primary VM's call to HF_VCPU_RUN. */
	arch_regs_set_retval(&next->regs,
			     hf_vcpu_run_return_encode(primary_ret));

	/* Mark the current vcpu as waiting. */
	sl_lock(&current->lock);
	current->state = secondary_state;
	sl_unlock(&current->lock);

	return next;
}

/**
 * Returns to the primary vm and signals that the vcpu still has work to do so.
 */
struct vcpu *api_preempt(struct vcpu *current)
{
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_PREEMPTED,
	};

	return api_switch_to_primary(current, ret, vcpu_state_ready);
}

/**
 * Puts the current vcpu in wait for interrupt mode, and returns to the primary
 * vm.
 */
struct vcpu *api_wait_for_interrupt(struct vcpu *current)
{
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_WAIT_FOR_INTERRUPT,
	};

	return api_switch_to_primary(current, ret,
				     vcpu_state_blocked_interrupt);
}

/**
 * Returns to the primary vm to allow this cpu to be used for other tasks as the
 * vcpu does not have work to do at this moment. The current vcpu is marked as
 * ready to be scheduled again.
 */
struct vcpu *api_yield(struct vcpu *current)
{
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_YIELD,
	};

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/* Noop on the primary as it makes the scheduling decisions.  */
		return NULL;
	}

	return api_switch_to_primary(current, ret, vcpu_state_ready);
}

/**
 * Aborts the vCPU and triggers its VM to abort fully.
 */
struct vcpu *api_abort(struct vcpu *current)
{
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_ABORTED,
	};

	dlog("Aborting VM %u vCPU %u\n", current->vm->id, vcpu_index(current));

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/* TODO: what to do when the primary aborts? */
		for (;;) {
			/* Do nothing. */
		}
	}

	atomic_store_explicit(&current->vm->aborting, true,
			      memory_order_relaxed);

	/* TODO: free resources once all vCPUs abort. */

	return api_switch_to_primary(current, ret, vcpu_state_aborted);
}

/**
 * Returns the ID of the VM.
 */
int64_t api_vm_get_id(const struct vcpu *current)
{
	return current->vm->id;
}

/**
 * Returns the number of VMs configured to run.
 */
int64_t api_vm_get_count(void)
{
	return vm_get_count();
}

/**
 * Returns the number of vcpus configured in the given VM.
 */
int64_t api_vcpu_get_count(uint32_t vm_id, const struct vcpu *current)
{
	struct vm *vm;

	/* Only the primary VM needs to know about vcpus for scheduling. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		return -1;
	}

	vm = vm_get(vm_id);
	if (vm == NULL) {
		return -1;
	}

	return vm->vcpu_count;
}

/**
 * This function is called by the architecture-specific context switching
 * function to indicate that register state for the given vcpu has been saved
 * and can therefore be used by other pcpus.
 */
void api_regs_state_saved(struct vcpu *vcpu)
{
	sl_lock(&vcpu->lock);
	vcpu->regs_available = true;
	sl_unlock(&vcpu->lock);
}

/**
 * Retrieves the next waiter and removes it from the wait list if the VM's
 * mailbox is in a writable state.
 */
static struct wait_entry *api_fetch_waiter(struct vm_locked locked_vm)
{
	struct wait_entry *entry;
	struct vm *vm = locked_vm.vm;

	if (vm->mailbox.state != mailbox_state_empty ||
	    vm->mailbox.recv == NULL || list_empty(&vm->mailbox.waiter_list)) {
		/* The mailbox is not writable or there are no waiters. */
		return NULL;
	}

	/* Remove waiter from the wait list. */
	entry = CONTAINER_OF(vm->mailbox.waiter_list.next, struct wait_entry,
			     wait_links);
	list_remove(&entry->wait_links);
	return entry;
}

/**
 * Assuming that the arguments have already been checked by the caller, injects
 * a virtual interrupt of the given ID into the given target vCPU. This doesn't
 * cause the vCPU to actually be run immediately; it will be taken when the vCPU
 * is next run, which is up to the scheduler.
 *
 * Returns:
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick the target vCPU.
 */
static int64_t internal_interrupt_inject(struct vm *target_vm,
					 struct vcpu *target_vcpu,
					 uint32_t intid, struct vcpu *current,
					 struct vcpu **next)
{
	uint32_t intid_index = intid / INTERRUPT_REGISTER_BITS;
	uint32_t intid_mask = 1u << (intid % INTERRUPT_REGISTER_BITS);
	bool need_vm_lock;
	int64_t ret = 0;

	sl_lock(&target_vcpu->lock);
	/*
	 * If we need the target_vm lock we need to release the target_vcpu lock
	 * first to maintain the correct order of locks. In-between releasing
	 * and acquiring it again the state of the vCPU could change in such a
	 * way that we don't actually need to touch the target_vm after all, but
	 * that's alright: we'll take the target_vm lock anyway, but it's safe,
	 * just perhaps a little slow in this unusual case. The reverse is not
	 * possible: if need_vm_lock is false, we don't release the target_vcpu
	 * lock until we are done, so nothing should change in such as way that
	 * we need the VM lock after all.
	 */
	need_vm_lock =
		(target_vcpu->interrupts.interrupt_enabled[intid_index] &
		 ~target_vcpu->interrupts.interrupt_pending[intid_index] &
		 intid_mask) &&
		target_vcpu->state == vcpu_state_blocked_mailbox;
	if (need_vm_lock) {
		sl_unlock(&target_vcpu->lock);
		sl_lock(&target_vm->lock);
		sl_lock(&target_vcpu->lock);
	}

	/*
	 * We only need to change state and (maybe) trigger a virtual IRQ if it
	 * is enabled and was not previously pending. Otherwise we can skip
	 * everything except setting the pending bit.
	 *
	 * If you change this logic make sure to update the need_vm_lock logic
	 * above to match.
	 */
	if (!(target_vcpu->interrupts.interrupt_enabled[intid_index] &
	      ~target_vcpu->interrupts.interrupt_pending[intid_index] &
	      intid_mask)) {
		goto out;
	}

	/* Increment the count. */
	target_vcpu->interrupts.enabled_and_pending_count++;

	/*
	 * Only need to update state if there was not already an
	 * interrupt enabled and pending.
	 */
	if (target_vcpu->interrupts.enabled_and_pending_count != 1) {
		goto out;
	}

	if (target_vcpu->state == vcpu_state_blocked_interrupt) {
		target_vcpu->state = vcpu_state_ready;
	} else if (target_vcpu->state == vcpu_state_blocked_mailbox) {
		/*
		 * need_vm_lock must be true if this path is taken, so if you
		 * change the condition here or those leading up to it make sure
		 * to update the need_vm_lock logic above to match.
		 */

		/* Take target vCPU out of mailbox recv_waiter list. */
		/*
		 * TODO: Consider using a doubly-linked list for the receive
		 * waiter list to avoid the linear search here.
		 */
		struct vcpu **previous_next_pointer =
			&target_vm->mailbox.recv_waiter;
		while (*previous_next_pointer != NULL &&
		       *previous_next_pointer != target_vcpu) {
			/*
			 * TODO(qwandor): Do we need to lock the vCPUs somehow
			 * while we walk the linked list, or is the VM lock
			 * enough?
			 */
			previous_next_pointer =
				&(*previous_next_pointer)->mailbox_next;
		}
		if (*previous_next_pointer == NULL) {
			dlog("Target VCPU state is vcpu_state_blocked_mailbox "
			     "but is not in VM mailbox waiter list. This "
			     "should never happen.\n");
		} else {
			*previous_next_pointer = target_vcpu->mailbox_next;
		}

		target_vcpu->state = vcpu_state_ready;
	}

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/*
		 * If the call came from the primary VM, let it know that it
		 * should run or kick the target vCPU.
		 */
		ret = 1;
	} else if (current != target_vcpu && next != NULL) {
		/*
		 * Switch to the primary so that it can switch to the target, or
		 * kick it if it is already running on a different physical CPU.
		 */
		struct hf_vcpu_run_return ret = {
			.code = HF_VCPU_RUN_WAKE_UP,
			.wake_up.vm_id = target_vm->id,
			.wake_up.vcpu = target_vcpu - target_vm->vcpus,
		};
		*next = api_switch_to_primary(current, ret, vcpu_state_ready);
	}

out:
	/* Either way, make it pending. */
	target_vcpu->interrupts.interrupt_pending[intid_index] |= intid_mask;

	sl_unlock(&target_vcpu->lock);
	if (need_vm_lock) {
		sl_unlock(&target_vm->lock);
	}

	return ret;
}

/**
 * Prepares the vcpu to run by updating its state and fetching whether a return
 * value needs to be forced onto the vCPU.
 */
static bool api_vcpu_prepare_run(const struct vcpu *current, struct vcpu *vcpu,
				 struct retval_state *vcpu_retval,
				 struct hf_vcpu_run_return *run_ret)
{
	bool ret;

	sl_lock(&vcpu->lock);

	if (atomic_load_explicit(&vcpu->vm->aborting, memory_order_relaxed)) {
		if (vcpu->state != vcpu_state_aborted) {
			dlog("Aborting VM %u vCPU %u\n", vcpu->vm->id,
			     vcpu_index(vcpu));
			vcpu->state = vcpu_state_aborted;
		}
		ret = false;
		goto out;
	}

	/*
	 * Wait until the registers become available. Care must be taken when
	 * looping on this: it shouldn't be done while holding other locks to
	 * avoid deadlocks.
	 */
	while (!vcpu->regs_available) {
		if (vcpu->state == vcpu_state_running) {
			/*
			 * vCPU is running on another pCPU.
			 *
			 * It's ok to not return HF_VCPU_RUN_SLEEP here because
			 * the other physical CPU that is currently running this
			 * vcpu will return HF_VCPU_RUN_SLEEP if neeed. The
			 * default return value is
			 * HF_VCPU_RUN_WAIT_FOR_INTERRUPT, so no need to set it
			 * explicitly.
			 */
			ret = false;
			goto out;
		}

		sl_unlock(&vcpu->lock);
		sl_lock(&vcpu->lock);
	}

	switch (vcpu->state) {
	case vcpu_state_running:
	case vcpu_state_off:
	case vcpu_state_aborted:
		ret = false;
		goto out;
	case vcpu_state_blocked_interrupt:
	case vcpu_state_blocked_mailbox:
		if (arch_timer_pending(&vcpu->regs)) {
			break;
		}

		/*
		 * The vCPU is not ready to run, return the appropriate code to
		 * the primary which called vcpu_run.
		 */
		if (arch_timer_enabled(&vcpu->regs)) {
			run_ret->code = HF_VCPU_RUN_SLEEP;
			run_ret->sleep.ns =
				arch_timer_remaining_ns(&vcpu->regs);
		}

		ret = false;
		goto out;
	case vcpu_state_ready:
		break;
	}
	/*
	 * If we made it to here then either the state was vcpu_state_ready or
	 * the timer is pending, so the vCPU should run to handle the timer
	 * firing.
	 */

	vcpu->cpu = current->cpu;
	vcpu->state = vcpu_state_running;

	/* Fetch return value to inject into vCPU if there is one. */
	*vcpu_retval = vcpu->retval;
	if (vcpu_retval->force) {
		vcpu->retval.force = false;
	}

	/*
	 * Mark the registers as unavailable now that we're about to reflect
	 * them onto the real registers. This will also prevent another physical
	 * CPU from trying to read these registers.
	 */
	vcpu->regs_available = false;

	ret = true;

out:
	sl_unlock(&vcpu->lock);
	return ret;
}

/**
 * Runs the given vcpu of the given vm.
 */
struct hf_vcpu_run_return api_vcpu_run(uint32_t vm_id, uint32_t vcpu_idx,
				       const struct vcpu *current,
				       struct vcpu **next)
{
	struct vm *vm;
	struct vcpu *vcpu;
	struct retval_state vcpu_retval;
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_WAIT_FOR_INTERRUPT,
	};

	/* Only the primary VM can switch vcpus. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		goto out;
	}

	/* Only secondary VM vcpus can be run. */
	if (vm_id == HF_PRIMARY_VM_ID) {
		goto out;
	}

	/* The requested VM must exist. */
	vm = vm_get(vm_id);
	if (vm == NULL) {
		goto out;
	}

	/* The requested vcpu must exist. */
	if (vcpu_idx >= vm->vcpu_count) {
		goto out;
	}

	/* Update state if allowed. */
	vcpu = &vm->vcpus[vcpu_idx];
	if (!api_vcpu_prepare_run(current, vcpu, &vcpu_retval, &ret)) {
		goto out;
	}

	/*
	 * Inject timer interrupt if timer has expired. It's safe to access
	 * vcpu->regs here because api_vcpu_prepare_run already made sure that
	 * regs_available was true (and then set it to false) before returning
	 * true.
	 */
	if (arch_timer_pending(&vcpu->regs)) {
		/* Make virtual timer interrupt pending. */
		internal_interrupt_inject(vm, vcpu, HF_VIRTUAL_TIMER_INTID,
					  vcpu, NULL);

		/*
		 * Set the mask bit so the hardware interrupt doesn't fire
		 * again. Ideally we wouldn't do this because it affects what
		 * the secondary vCPU sees, but if we don't then we end up with
		 * a loop of the interrupt firing each time we try to return to
		 * the secondary vCPU.
		 */
		arch_timer_mask(&vcpu->regs);
	}

	/* Switch to the vcpu. */
	*next = vcpu;

	/*
	 * Set a placeholder return code to the scheduler. This will be
	 * overwritten when the switch back to the primary occurs.
	 */
	ret.code = HF_VCPU_RUN_PREEMPTED;

	/* Update return value for the next vcpu if one was injected. */
	if (vcpu_retval.force) {
		arch_regs_set_retval(&vcpu->regs, vcpu_retval.value);
	}

out:
	return ret;
}

/**
 * Check that the mode indicates memory that is valid, owned and exclusive.
 */
static bool api_mode_valid_owned_and_exclusive(int mode)
{
	return (mode & (MM_MODE_INVALID | MM_MODE_UNOWNED | MM_MODE_SHARED)) ==
	       0;
}

/**
 * Determines the value to be returned by api_vm_configure and api_mailbox_clear
 * after they've succeeded. If a secondary VM is running and there are waiters,
 * it also switches back to the primary VM for it to wake waiters up.
 */
static int64_t api_waiter_result(struct vm_locked locked_vm,
				 struct vcpu *current, struct vcpu **next)
{
	struct vm *vm = locked_vm.vm;
	struct hf_vcpu_run_return ret = {
		.code = HF_VCPU_RUN_NOTIFY_WAITERS,
	};

	if (list_empty(&vm->mailbox.waiter_list)) {
		/* No waiters, nothing else to do. */
		return 0;
	}

	if (vm->id == HF_PRIMARY_VM_ID) {
		/* The caller is the primary VM. Tell it to wake up waiters. */
		return 1;
	}

	/*
	 * Switch back to the primary VM, informing it that there are waiters
	 * that need to be notified.
	 */
	*next = api_switch_to_primary(current, ret, vcpu_state_ready);

	return 0;
}

/**
 * Configures the VM to send/receive data through the specified pages. The pages
 * must not be shared.
 *
 * Returns:
 *  - -1 on failure.
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick waiters. Waiters should be retrieved by calling
 *    hf_mailbox_waiter_get.
 */
int64_t api_vm_configure(ipaddr_t send, ipaddr_t recv, struct vcpu *current,
			 struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct vm_locked locked;
	paddr_t pa_send_begin;
	paddr_t pa_send_end;
	paddr_t pa_recv_begin;
	paddr_t pa_recv_end;
	int orig_send_mode;
	int orig_recv_mode;
	struct mpool local_page_pool;
	int64_t ret;

	/* Fail if addresses are not page-aligned. */
	if (!is_aligned(ipa_addr(send), PAGE_SIZE) ||
	    !is_aligned(ipa_addr(recv), PAGE_SIZE)) {
		return -1;
	}

	/* Convert to physical addresses. */
	pa_send_begin = pa_from_ipa(send);
	pa_send_end = pa_add(pa_send_begin, PAGE_SIZE);

	pa_recv_begin = pa_from_ipa(recv);
	pa_recv_end = pa_add(pa_recv_begin, PAGE_SIZE);

	/* Fail if the same page is used for the send and receive pages. */
	if (pa_addr(pa_send_begin) == pa_addr(pa_recv_begin)) {
		return -1;
	}

	vm_lock(vm, &locked);

	/* We only allow these to be setup once. */
	if (vm->mailbox.send || vm->mailbox.recv) {
		goto fail;
	}

	/*
	 * Ensure the pages are valid, owned and exclusive to the VM and that
	 * the VM has the required access to the memory.
	 */
	if (!mm_vm_get_mode(&vm->ptable, send, ipa_add(send, PAGE_SIZE),
			    &orig_send_mode) ||
	    !api_mode_valid_owned_and_exclusive(orig_send_mode) ||
	    (orig_send_mode & MM_MODE_R) == 0 ||
	    (orig_send_mode & MM_MODE_W) == 0) {
		goto fail;
	}

	if (!mm_vm_get_mode(&vm->ptable, recv, ipa_add(recv, PAGE_SIZE),
			    &orig_recv_mode) ||
	    !api_mode_valid_owned_and_exclusive(orig_recv_mode) ||
	    (orig_recv_mode & MM_MODE_R) == 0) {
		goto fail;
	}

	/*
	 * Create a local pool so any freed memory can't be used by another
	 * thread. This is to ensure the original mapping can be restored if any
	 * stage of the process fails.
	 */
	mpool_init_with_fallback(&local_page_pool, &api_page_pool);

	/* Take memory ownership away from the VM and mark as shared. */
	if (!mm_vm_identity_map(
		    &vm->ptable, pa_send_begin, pa_send_end,
		    MM_MODE_UNOWNED | MM_MODE_SHARED | MM_MODE_R | MM_MODE_W,
		    NULL, &local_page_pool)) {
		goto fail_free_pool;
	}

	if (!mm_vm_identity_map(&vm->ptable, pa_recv_begin, pa_recv_end,
				MM_MODE_UNOWNED | MM_MODE_SHARED | MM_MODE_R,
				NULL, &local_page_pool)) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_vm_defrag(&vm->ptable, &local_page_pool);
		goto fail_undo_send;
	}

	/* Map the send page as read-only in the hypervisor address space. */
	vm->mailbox.send = mm_identity_map(pa_send_begin, pa_send_end,
					   MM_MODE_R, &local_page_pool);
	if (!vm->mailbox.send) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(&local_page_pool);
		goto fail_undo_send_and_recv;
	}

	/*
	 * Map the receive page as writable in the hypervisor address space. On
	 * failure, unmap the send page before returning.
	 */
	vm->mailbox.recv = mm_identity_map(pa_recv_begin, pa_recv_end,
					   MM_MODE_W, &local_page_pool);
	if (!vm->mailbox.recv) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(&local_page_pool);
		goto fail_undo_all;
	}

	/* Tell caller about waiters, if any. */
	ret = api_waiter_result(locked, current, next);
	goto exit;

	/*
	 * The following mappings will not require more memory than is available
	 * in the local pool.
	 */
fail_undo_all:
	vm->mailbox.send = NULL;
	mm_unmap(pa_send_begin, pa_send_end, &local_page_pool);

fail_undo_send_and_recv:
	mm_vm_identity_map(&vm->ptable, pa_recv_begin, pa_recv_end,
			   orig_recv_mode, NULL, &local_page_pool);

fail_undo_send:
	mm_vm_identity_map(&vm->ptable, pa_send_begin, pa_send_end,
			   orig_send_mode, NULL, &local_page_pool);

fail_free_pool:
	mpool_fini(&local_page_pool);

fail:
	ret = -1;

exit:
	vm_unlock(&locked);

	return ret;
}

/**
 * Copies data from the sender's send buffer to the recipient's receive buffer
 * and notifies the recipient.
 *
 * If the recipient's receive buffer is busy, it can optionally register the
 * caller to be notified when the recipient's receive buffer becomes available.
 */
int64_t api_mailbox_send(uint32_t vm_id, size_t size, bool notify,
			 struct vcpu *current, struct vcpu **next)
{
	struct vm *from = current->vm;
	struct vm *to;
	const void *from_buf;
	uint16_t vcpu;
	int64_t ret;

	/* Limit the size of transfer. */
	if (size > HF_MAILBOX_SIZE) {
		return -1;
	}

	/* Disallow reflexive requests as this suggests an error in the VM. */
	if (vm_id == from->id) {
		return -1;
	}

	/* Ensure the target VM exists. */
	to = vm_get(vm_id);
	if (to == NULL) {
		return -1;
	}

	/*
	 * Check that the sender has configured its send buffer. It is safe to
	 * use from_buf after releasing the lock because the buffer cannot be
	 * modified once it's configured.
	 */
	sl_lock(&from->lock);
	from_buf = from->mailbox.send;
	sl_unlock(&from->lock);
	if (from_buf == NULL) {
		return -1;
	}

	sl_lock(&to->lock);

	if (to->mailbox.state != mailbox_state_empty ||
	    to->mailbox.recv == NULL) {
		/*
		 * Fail if the target isn't currently ready to receive data,
		 * setting up for notification if requested.
		 */
		if (notify) {
			struct wait_entry *entry =
				&current->vm->wait_entries[vm_id];

			/* Append waiter only if it's not there yet. */
			if (list_empty(&entry->wait_links)) {
				list_append(&to->mailbox.waiter_list,
					    &entry->wait_links);
			}
		}

		ret = -1;
		goto out;
	}

	/* Copy data. */
	memcpy(to->mailbox.recv, from_buf, size);
	to->mailbox.recv_bytes = size;
	to->mailbox.recv_from_id = from->id;
	to->mailbox.state = mailbox_state_read;

	/* Messages for the primary VM are delivered directly. */
	if (to->id == HF_PRIMARY_VM_ID) {
		struct hf_vcpu_run_return primary_ret = {
			.code = HF_VCPU_RUN_MESSAGE,
			.message.size = size,
		};

		*next = api_switch_to_primary(current, primary_ret,
					      vcpu_state_ready);
		ret = 0;
		goto out;
	}

	/*
	 * Try to find a vcpu to handle the message and tell the scheduler to
	 * run it.
	 */
	if (to->mailbox.recv_waiter == NULL) {
		/*
		 * The scheduler must choose a vcpu to interrupt so it can
		 * handle the message.
		 */
		to->mailbox.state = mailbox_state_received;
		vcpu = HF_INVALID_VCPU;
	} else {
		struct vcpu *to_vcpu = to->mailbox.recv_waiter;

		/*
		 * Take target vcpu out of waiter list and mark it as ready to
		 * run again.
		 */
		sl_lock(&to_vcpu->lock);
		to->mailbox.recv_waiter = to_vcpu->mailbox_next;
		to_vcpu->state = vcpu_state_ready;

		/* Return from HF_MAILBOX_RECEIVE. */
		to_vcpu->retval.force = true;
		to_vcpu->retval.value = hf_mailbox_receive_return_encode(
			(struct hf_mailbox_receive_return){
				.vm_id = to->mailbox.recv_from_id,
				.size = size,
			});

		sl_unlock(&to_vcpu->lock);

		vcpu = to_vcpu - to->vcpus;
	}

	/* Return to the primary VM directly or with a switch. */
	if (from->id == HF_PRIMARY_VM_ID) {
		ret = vcpu;
	} else {
		struct hf_vcpu_run_return primary_ret = {
			.code = HF_VCPU_RUN_WAKE_UP,
			.wake_up.vm_id = to->id,
			.wake_up.vcpu = vcpu,
		};

		*next = api_switch_to_primary(current, primary_ret,
					      vcpu_state_ready);
		ret = 0;
	}

out:
	sl_unlock(&to->lock);

	return ret;
}

/**
 * Receives a message from the mailbox. If one isn't available, this function
 * can optionally block the caller until one becomes available.
 *
 * No new messages can be received until the mailbox has been cleared.
 */
struct hf_mailbox_receive_return api_mailbox_receive(bool block,
						     struct vcpu *current,
						     struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct hf_mailbox_receive_return ret = {
		.vm_id = HF_INVALID_VM_ID,
	};

	/*
	 * The primary VM will receive messages as a status code from running
	 * vcpus and must not call this function.
	 */
	if (vm->id == HF_PRIMARY_VM_ID) {
		return ret;
	}

	sl_lock(&vm->lock);

	/* Return pending messages without blocking. */
	if (vm->mailbox.state == mailbox_state_received) {
		vm->mailbox.state = mailbox_state_read;
		ret.vm_id = vm->mailbox.recv_from_id;
		ret.size = vm->mailbox.recv_bytes;
		goto out;
	}

	/*
	 * No pending message so fail if not allowed to block. Don't block if
	 * there are enabled and pending interrupts, to match behaviour of
	 * wait_for_interrupt.
	 */
	if (!block || current->interrupts.enabled_and_pending_count > 0) {
		goto out;
	}

	sl_lock(&current->lock);

	/* Push vcpu into waiter list. */
	current->mailbox_next = vm->mailbox.recv_waiter;
	vm->mailbox.recv_waiter = current;
	sl_unlock(&current->lock);

	/* Switch back to primary vm to block. */
	{
		struct hf_vcpu_run_return run_return = {
			.code = HF_VCPU_RUN_WAIT_FOR_INTERRUPT,
		};

		*next = api_switch_to_primary(current, run_return,
					      vcpu_state_blocked_mailbox);
	}
out:
	sl_unlock(&vm->lock);

	return ret;
}

/**
 * Retrieves the next VM whose mailbox became writable. For a VM to be notified
 * by this function, the caller must have called api_mailbox_send before with
 * the notify argument set to true, and this call must have failed because the
 * mailbox was not available.
 *
 * It should be called repeatedly to retrieve a list of VMs.
 *
 * Returns -1 if no VM became writable, or the id of the VM whose mailbox
 * became writable.
 */
int64_t api_mailbox_writable_get(const struct vcpu *current)
{
	struct vm *vm = current->vm;
	struct wait_entry *entry;
	int64_t ret;

	sl_lock(&vm->lock);
	if (list_empty(&vm->mailbox.ready_list)) {
		ret = -1;
		goto exit;
	}

	entry = CONTAINER_OF(vm->mailbox.ready_list.next, struct wait_entry,
			     ready_links);
	list_remove(&entry->ready_links);
	ret = entry - vm->wait_entries;

exit:
	sl_unlock(&vm->lock);
	return ret;
}

/**
 * Retrieves the next VM waiting to be notified that the mailbox of the
 * specified VM became writable. Only primary VMs are allowed to call this.
 *
 * Returns -1 on failure or if there are no waiters; the VM id of the next
 * waiter otherwise.
 */
int64_t api_mailbox_waiter_get(uint32_t vm_id, const struct vcpu *current)
{
	struct vm *vm;
	struct vm_locked locked;
	struct wait_entry *entry;
	struct vm *waiting_vm;

	/* Only primary VMs are allowed to call this function. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		return -1;
	}

	vm = vm_get(vm_id);
	if (vm == NULL) {
		return -1;
	}

	/* Check if there are outstanding notifications from given vm. */
	vm_lock(vm, &locked);
	entry = api_fetch_waiter(locked);
	vm_unlock(&locked);

	if (entry == NULL) {
		return -1;
	}

	/* Enqueue notification to waiting VM. */
	waiting_vm = entry->waiting_vm;

	sl_lock(&waiting_vm->lock);
	if (list_empty(&entry->ready_links)) {
		list_append(&waiting_vm->mailbox.ready_list,
			    &entry->ready_links);
	}
	sl_unlock(&waiting_vm->lock);

	return waiting_vm->id;
}

/**
 * Clears the caller's mailbox so that a new message can be received. The caller
 * must have copied out all data they wish to preserve as new messages will
 * overwrite the old and will arrive asynchronously.
 *
 * Returns:
 *  - -1 on failure, if the mailbox hasn't been read.
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick waiters. Waiters should be retrieved by calling
 *    hf_mailbox_waiter_get.
 */
int64_t api_mailbox_clear(struct vcpu *current, struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct vm_locked locked;
	int64_t ret;

	vm_lock(vm, &locked);
	switch (vm->mailbox.state) {
	case mailbox_state_empty:
		ret = 0;
		break;

	case mailbox_state_received:
		ret = -1;
		break;

	case mailbox_state_read:
		ret = api_waiter_result(locked, current, next);
		vm->mailbox.state = mailbox_state_empty;
		break;
	}
	vm_unlock(&locked);

	return ret;
}

/**
 * Enables or disables a given interrupt ID for the calling vCPU.
 *
 * Returns 0 on success, or -1 if the intid is invalid.
 */
int64_t api_interrupt_enable(uint32_t intid, bool enable, struct vcpu *current)
{
	uint32_t intid_index = intid / INTERRUPT_REGISTER_BITS;
	uint32_t intid_mask = 1u << (intid % INTERRUPT_REGISTER_BITS);

	if (intid >= HF_NUM_INTIDS) {
		return -1;
	}

	sl_lock(&current->lock);
	if (enable) {
		/*
		 * If it is pending and was not enabled before, increment the
		 * count.
		 */
		if (current->interrupts.interrupt_pending[intid_index] &
		    ~current->interrupts.interrupt_enabled[intid_index] &
		    intid_mask) {
			current->interrupts.enabled_and_pending_count++;
		}
		current->interrupts.interrupt_enabled[intid_index] |=
			intid_mask;
	} else {
		/*
		 * If it is pending and was enabled before, decrement the count.
		 */
		if (current->interrupts.interrupt_pending[intid_index] &
		    current->interrupts.interrupt_enabled[intid_index] &
		    intid_mask) {
			current->interrupts.enabled_and_pending_count--;
		}
		current->interrupts.interrupt_enabled[intid_index] &=
			~intid_mask;
	}

	sl_unlock(&current->lock);
	return 0;
}

/**
 * Returns the ID of the next pending interrupt for the calling vCPU, and
 * acknowledges it (i.e. marks it as no longer pending). Returns
 * HF_INVALID_INTID if there are no pending interrupts.
 */
uint32_t api_interrupt_get(struct vcpu *current)
{
	uint8_t i;
	uint32_t first_interrupt = HF_INVALID_INTID;

	/*
	 * Find the first enabled and pending interrupt ID, return it, and
	 * deactivate it.
	 */
	sl_lock(&current->lock);
	for (i = 0; i < HF_NUM_INTIDS / INTERRUPT_REGISTER_BITS; ++i) {
		uint32_t enabled_and_pending =
			current->interrupts.interrupt_enabled[i] &
			current->interrupts.interrupt_pending[i];

		if (enabled_and_pending != 0) {
			uint8_t bit_index = ctz(enabled_and_pending);
			/*
			 * Mark it as no longer pending and decrement the count.
			 */
			current->interrupts.interrupt_pending[i] &=
				~(1u << bit_index);
			current->interrupts.enabled_and_pending_count--;
			first_interrupt =
				i * INTERRUPT_REGISTER_BITS + bit_index;
			break;
		}
	}

	sl_unlock(&current->lock);
	return first_interrupt;
}

/**
 * Returns whether the current vCPU is allowed to inject an interrupt into the
 * given VM and vCPU.
 */
static inline bool is_injection_allowed(uint32_t target_vm_id,
					struct vcpu *current)
{
	uint32_t current_vm_id = current->vm->id;

	/*
	 * The primary VM is allowed to inject interrupts into any VM. Secondary
	 * VMs are only allowed to inject interrupts into their own vCPUs.
	 */
	return current_vm_id == HF_PRIMARY_VM_ID ||
	       current_vm_id == target_vm_id;
}

/**
 * Injects a virtual interrupt of the given ID into the given target vCPU.
 * This doesn't cause the vCPU to actually be run immediately; it will be taken
 * when the vCPU is next run, which is up to the scheduler.
 *
 * Returns:
 *  - -1 on failure because the target VM or vCPU doesn't exist, the interrupt
 *    ID is invalid, or the current VM is not allowed to inject interrupts to
 *    the target VM.
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick the target vCPU.
 */
int64_t api_interrupt_inject(uint32_t target_vm_id, uint32_t target_vcpu_idx,
			     uint32_t intid, struct vcpu *current,
			     struct vcpu **next)
{
	struct vcpu *target_vcpu;
	struct vm *target_vm = vm_get(target_vm_id);

	if (intid >= HF_NUM_INTIDS) {
		return -1;
	}

	if (target_vm == NULL) {
		return -1;
	}

	if (target_vcpu_idx >= target_vm->vcpu_count) {
		/* The requested vcpu must exist. */
		return -1;
	}

	if (!is_injection_allowed(target_vm_id, current)) {
		return -1;
	}

	target_vcpu = &target_vm->vcpus[target_vcpu_idx];

	dlog("Injecting IRQ %d for VM %d VCPU %d from VM %d VCPU %d\n", intid,
	     target_vm_id, target_vcpu_idx, current->vm->id, current->cpu->id);
	return internal_interrupt_inject(target_vm, target_vcpu, intid, current,
					 next);
}

/**
 * Clears a region of physical memory by overwriting it with zeros. The data is
 * flushed from the cache so the memory has been cleared across the system.
 */
static bool api_clear_memory(paddr_t begin, paddr_t end, struct mpool *ppool)
{
	/*
	 * TODO: change this to a cpu local single page window rather than a
	 *       global mapping of the whole range. Such an approach will limit
	 *       the changes to stage-1 tables and will allow only local
	 *       invalidation.
	 */
	void *ptr = mm_identity_map(begin, end, MM_MODE_W, ppool);
	size_t size = pa_addr(end) - pa_addr(begin);

	if (!ptr) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(ppool);
		return false;
	}

	memset(ptr, 0, size);
	arch_mm_write_back_dcache(ptr, size);
	mm_unmap(begin, end, ppool);

	return true;
}

/**
 * Shares memory from the calling VM with another. The memory can be shared in
 * different modes.
 *
 * TODO: the interface for sharing memory will need to be enhanced to allow
 *       sharing with different modes e.g. read-only, informing the recipient
 *       of the memory they have been given, opting to not wipe the memory and
 *       possibly allowing multiple blocks to be transferred. What this will
 *       look like is TBD.
 */
int64_t api_share_memory(uint32_t vm_id, ipaddr_t addr, size_t size,
			 enum hf_share share, struct vcpu *current)
{
	struct vm *from = current->vm;
	struct vm *to;
	int orig_from_mode;
	int from_mode;
	int to_mode;
	ipaddr_t begin;
	ipaddr_t end;
	paddr_t pa_begin;
	paddr_t pa_end;
	struct mpool local_page_pool;
	int64_t ret;

	/* Disallow reflexive shares as this suggests an error in the VM. */
	if (vm_id == from->id) {
		return -1;
	}

	/* Ensure the target VM exists. */
	to = vm_get(vm_id);
	if (to == NULL) {
		return -1;
	}

	begin = addr;
	end = ipa_add(addr, size);

	/* Fail if addresses are not page-aligned. */
	if (!is_aligned(ipa_addr(begin), PAGE_SIZE) ||
	    !is_aligned(ipa_addr(end), PAGE_SIZE)) {
		return -1;
	}

	/* Convert the sharing request to memory management modes. */
	switch (share) {
	case HF_MEMORY_GIVE:
		from_mode = MM_MODE_INVALID | MM_MODE_UNOWNED;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X;
		break;

	case HF_MEMORY_LEND:
		from_mode = MM_MODE_INVALID;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_UNOWNED;
		break;

	case HF_MEMORY_SHARE:
		from_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_SHARED;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_UNOWNED |
			  MM_MODE_SHARED;
		break;

	default:
		/* The input is untrusted so might not be a valid value. */
		return -1;
	}

	/*
	 * Create a local pool so any freed memory can't be used by another
	 * thread. This is to ensure the original mapping can be restored if any
	 * stage of the process fails.
	 */
	mpool_init_with_fallback(&local_page_pool, &api_page_pool);

	sl_lock_both(&from->lock, &to->lock);

	/*
	 * Ensure that the memory range is mapped with the same mode so that
	 * changes can be reverted if the process fails.
	 */
	if (!mm_vm_get_mode(&from->ptable, begin, end, &orig_from_mode)) {
		goto fail;
	}

	/*
	 * Ensure the memory range is valid for the sender. If it isn't, the
	 * sender has either shared it with another VM already or has no claim
	 * to the memory.
	 */
	if (orig_from_mode & MM_MODE_INVALID) {
		goto fail;
	}

	/*
	 * The sender must own the memory and have exclusive access to it in
	 * order to share it. Alternatively, it is giving memory back to the
	 * owning VM.
	 */
	if (orig_from_mode & MM_MODE_UNOWNED) {
		int orig_to_mode;

		if (share != HF_MEMORY_GIVE ||
		    !mm_vm_get_mode(&to->ptable, begin, end, &orig_to_mode) ||
		    orig_to_mode & MM_MODE_UNOWNED) {
			goto fail;
		}
	} else if (orig_from_mode & MM_MODE_SHARED) {
		goto fail;
	}

	pa_begin = pa_from_ipa(begin);
	pa_end = pa_from_ipa(end);

	/*
	 * First update the mapping for the sender so there is not overlap with
	 * the recipient.
	 */
	if (!mm_vm_identity_map(&from->ptable, pa_begin, pa_end, from_mode,
				NULL, &local_page_pool)) {
		goto fail;
	}

	/* Clear the memory so no VM or device can see the previous contents. */
	if (!api_clear_memory(pa_begin, pa_end, &local_page_pool)) {
		goto fail_return_to_sender;
	}

	/* Complete the transfer by mapping the memory into the recipient. */
	if (!mm_vm_identity_map(&to->ptable, pa_begin, pa_end, to_mode, NULL,
				&local_page_pool)) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_vm_defrag(&from->ptable, &local_page_pool);
		goto fail_return_to_sender;
	}

	ret = 0;
	goto out;

fail_return_to_sender:
	mm_vm_identity_map(&from->ptable, pa_begin, pa_end, orig_from_mode,
			   NULL, &local_page_pool);

fail:
	ret = -1;

out:
	sl_unlock(&from->lock);
	sl_unlock(&to->lock);

	mpool_fini(&local_page_pool);

	return ret;
}

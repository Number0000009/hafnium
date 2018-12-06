/*
 * Copyright 2018 Google LLC
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

#pragma once

#include "hf/arch/cpu.h"

#include "hf/abi.h"
#include "hf/types.h"

/* Keep macro alignment */
/* clang-format off */

/* TODO: Define constants below according to spec. */
#define HF_VCPU_RUN                      0xff00
#define HF_VM_GET_COUNT                  0xff01
#define HF_VCPU_GET_COUNT                0xff02
#define HF_VM_CONFIGURE                  0xff03
#define HF_MAILBOX_SEND                  0xff04
#define HF_MAILBOX_RECEIVE               0xff05
#define HF_MAILBOX_CLEAR                 0xff06
#define HF_ENABLE_INTERRUPT              0xff07
#define HF_GET_AND_ACKNOWLEDGE_INTERRUPT 0xff08
#define HF_INJECT_INTERRUPT              0xff09

/** The amount of data that can be sent to a mailbox. */
#define HF_MAILBOX_SIZE 4096

/* clang-format on */

/**
 * This function must be implemented to trigger the architecture specific
 * mechanism to call to the hypervisor.
 */
int64_t hf_call(size_t arg0, size_t arg1, size_t arg2, size_t arg3);

/**
 * Runs the given vcpu of the given vm.
 *
 * Returns an hf_vcpu_run_return struct telling the scheduler what to do next.
 */
static inline struct hf_vcpu_run_return hf_vcpu_run(uint32_t vm_id,
						    uint32_t vcpu_idx)
{
	return hf_vcpu_run_return_decode(
		hf_call(HF_VCPU_RUN, vm_id, vcpu_idx, 0));
}

/**
 * Returns the number of secondary VMs.
 */
static inline int64_t hf_vm_get_count(void)
{
	return hf_call(HF_VM_GET_COUNT, 0, 0, 0);
}

/**
 * Returns the number of VCPUs configured in the given secondary VM.
 */
static inline int64_t hf_vcpu_get_count(uint32_t vm_id)
{
	return hf_call(HF_VCPU_GET_COUNT, vm_id, 0, 0);
}

/**
 * Configures the pages to send/receive data through. The pages must not be
 * shared.
 *
 * Returns 0 on success or -1 or failure.
 */
static inline int64_t hf_vm_configure(hf_ipaddr_t send, hf_ipaddr_t recv)
{
	return hf_call(HF_VM_CONFIGURE, send, recv, 0);
}

/**
 * Copies data from the sender's send buffer to the recipient's receive buffer.
 *
 * Returns -1 on failure, and on success either:
 * - 0, if the caller is a secondary VM
 * - the ID of the vCPU to run to receive the message, if the caller is the
 * primary VM.
 * - HF_INVALID_VCPU if the caller is the primary VM and no vCPUs on the target
 * VM are currently waiting to receive a message.
 */
static inline int64_t hf_mailbox_send(uint32_t vm_id, size_t size)
{
	return hf_call(HF_MAILBOX_SEND, vm_id, size, 0);
}

/**
 * Called by secondary VMs to receive a message. The call can optionally block
 * until a message is received.
 *
 * If no message was received, the VM ID will be HF_INVALID_VM_ID.
 *
 * The mailbox must be cleared before a new message can be received.
 */
static inline struct hf_mailbox_receive_return hf_mailbox_receive(bool block)
{
	return hf_mailbox_receive_return_decode(
		hf_call(HF_MAILBOX_RECEIVE, block, 0, 0));
}

/**
 * Clears the caller's mailbox so a new message can be received.
 *
 * Returns 0 on success, or -1 if the mailbox hasn't been read or is already
 * empty.
 */
static inline int64_t hf_mailbox_clear(void)
{
	return hf_call(HF_MAILBOX_CLEAR, 0, 0, 0);
}

/**
 * Enables or disables a given interrupt ID.
 *
 * Returns 0 on success, or -1 if the intid is invalid.
 */
static inline uint64_t hf_enable_interrupt(uint32_t intid, bool enable)
{
	return hf_call(HF_ENABLE_INTERRUPT, intid, enable, 0);
}

/**
 * Gets the ID of the pending interrupt (if any) and acknowledge it.
 *
 * Returns HF_INVALID_INTID if there are no pending interrupts.
 */
static inline uint32_t hf_get_and_acknowledge_interrupt()
{
	return hf_call(HF_GET_AND_ACKNOWLEDGE_INTERRUPT, 0, 0, 0);
}

/**
 * Injects a virtual interrupt of the given ID into the given target vCPU.
 * This doesn't cause the vCPU to actually be run immediately; it will be taken
 * when the vCPU is next run, which is up to the scheduler.
 *
 * Returns 0 on success, or -1 if the target VM or vCPU doesn't exist or
 * the interrupt ID is invalid.
 */
static inline int64_t hf_inject_interrupt(uint32_t target_vm_id,
					  uint32_t target_vcpu_idx,
					  uint32_t intid)
{
	return hf_call(HF_INJECT_INTERRUPT, target_vm_id, target_vcpu_idx,
		       intid);
}

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

#pragma once

#include "hf/cpu.h"
#include "hf/mpool.h"
#include "hf/vm.h"

#include "vmapi/hf/call.h"

void api_init(struct mpool *ppool);
int64_t api_vm_get_id(const struct vcpu *current);
int64_t api_vm_get_count(void);
int64_t api_vcpu_get_count(uint32_t vm_id, const struct vcpu *current);
void api_regs_state_saved(struct vcpu *vcpu);
struct hf_vcpu_run_return api_vcpu_run(uint32_t vm_id, uint32_t vcpu_idx,
				       const struct vcpu *current,
				       struct vcpu **next);
int64_t api_vm_configure(ipaddr_t send, ipaddr_t recv, struct vcpu *current,
			 struct vcpu **next);
int64_t api_mailbox_send(uint32_t vm_id, size_t size, bool notify,
			 struct vcpu *current, struct vcpu **next);
struct hf_mailbox_receive_return api_mailbox_receive(bool block,
						     struct vcpu *current,
						     struct vcpu **next);
int64_t api_mailbox_clear(struct vcpu *current, struct vcpu **next);
int64_t api_mailbox_writable_get(const struct vcpu *current);
int64_t api_mailbox_waiter_get(uint32_t vm_id, const struct vcpu *current);
int64_t api_share_memory(uint32_t vm_id, ipaddr_t addr, size_t size,
			 enum hf_share share, struct vcpu *current);

struct vcpu *api_preempt(struct vcpu *current);
struct vcpu *api_wait_for_interrupt(struct vcpu *current);
struct vcpu *api_yield(struct vcpu *current);
struct vcpu *api_abort(struct vcpu *current);

int64_t api_interrupt_enable(uint32_t intid, bool enable, struct vcpu *current);
uint32_t api_interrupt_get(struct vcpu *current);
int64_t api_interrupt_inject(uint32_t target_vm_id, uint32_t target_vcpu_idx,
			     uint32_t intid, struct vcpu *current,
			     struct vcpu **next);

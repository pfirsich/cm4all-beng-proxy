// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "ThreadSocketFilter.hxx"

class NopThreadSocketFilter final : public ThreadSocketFilterHandler {
public:
	/* virtual methods from class ThreadSocketFilterHandler */
	void Run(ThreadSocketFilterInternal &f) override;
};

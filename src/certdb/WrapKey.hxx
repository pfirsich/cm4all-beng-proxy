// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string_view>

template<typename> class AllocatedArray;
struct CertDatabaseConfig;

using WrapKeyBuffer = std::array<std::byte, 32>;
using WrapKeyView = std::span<const std::byte, 32>;

class WrapKey {
	WrapKeyBuffer key;

	constexpr WrapKey() noexcept = default;

public:
	explicit constexpr WrapKey(WrapKeyView src) noexcept {
		std::copy(src.begin(), src.end(), key.begin());
	}

	static WrapKey Make(const CertDatabaseConfig &config, std::string_view name);
	static std::pair<const char *, WrapKey> MakeDefault(const CertDatabaseConfig &config);

	AllocatedArray<std::byte> Encrypt(std::span<const std::byte> src);
	AllocatedArray<std::byte> Decrypt(std::span<const std::byte> src);
};

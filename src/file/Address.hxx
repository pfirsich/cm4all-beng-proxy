// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

class AllocatorPtr;
class MatchData;
struct DelegateAddress;

/**
 * The address of a local static file.
 */
struct FileAddress {
	const char *path;
	const char *deflated = nullptr;
	const char *gzipped = nullptr;

	/**
	 * Absolute path of a directory below which the other paths
	 * (#path, #deflated, #gzipped) are located.
	 */
	const char *base = nullptr;

	const char *content_type = nullptr;

	std::span<const std::byte> content_type_lookup{};

	const char *document_root = nullptr;

	DelegateAddress *delegate = nullptr;

	bool auto_gzipped = false;

	bool auto_brotli_path = false;

	/**
	 * The value of #TRANSLATE_EXPAND_PATH.  Only used by the
	 * translation cache.
	 */
	bool expand_path = false;

	/**
	 * The value of #TRANSLATE_EXPAND_DOCUMENT_ROOT.  Only used by the
	 * translation cache.
	 */
	bool expand_document_root = false;

	/**
	 * @param _path the new path pointer (taken as-is, no deep copy)
	 */
	explicit constexpr FileAddress(const char *_path) noexcept
		:path(_path)
	{
	}

	/**
	 * Copy from an existing #FileAddress instance, but override the
	 * path.
	 *
	 * @param _path the new path pointer (taken as-is, no deep copy)
	 */
	FileAddress(AllocatorPtr alloc, const FileAddress &src,
		    const char *_path) noexcept;

	FileAddress(AllocatorPtr alloc, const FileAddress &src) noexcept;

	FileAddress(const FileAddress &) = delete;
	FileAddress &operator=(const FileAddress &) = delete;

	[[gnu::pure]]
	bool HasQueryString() const noexcept {
		return false;
	}

	/**
	 * Throws std::runtime_error on error.
	 */
	void Check() const;

	[[gnu::pure]]
	bool IsValidBase() const noexcept;

	bool SplitBase(AllocatorPtr alloc, const char *suffix) noexcept;

	FileAddress *SaveBase(AllocatorPtr alloc,
			      std::string_view suffix) const noexcept;
	FileAddress *LoadBase(AllocatorPtr alloc,
			      std::string_view suffix) const noexcept;

	/**
	 * Does this address need to be expanded with Expand()?
	 */
	[[gnu::pure]]
	bool IsExpandable() const noexcept;

	/**
	 * Throws std::runtime_error on error.
	 */
	void Expand(AllocatorPtr alloc, const MatchData &match_data);
};

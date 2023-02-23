// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Address.hxx"
#include "uri/Base.hxx"
#include "uri/Compare.hxx"
#include "uri/PEscape.hxx"
#include "pexpand.hxx"
#include "AllocatorPtr.hxx"

#include <stdexcept>

#include <assert.h>
#include <string.h>

NfsAddress::NfsAddress(AllocatorPtr alloc, const NfsAddress &other)
	:server(alloc.Dup(other.server)),
	 export_name(alloc.Dup(other.export_name)),
	 path(alloc.Dup(other.path)),
	 content_type(alloc.CheckDup(other.content_type)),
	 content_type_lookup(alloc.Dup(other.content_type_lookup)),
	 expand_path(other.expand_path) {}

const char *
NfsAddress::GetId(AllocatorPtr alloc) const
{
	assert(server != nullptr);
	assert(export_name != nullptr);
	assert(path != nullptr);

	return alloc.Concat(server, ":", export_name, ":", path);
}

void
NfsAddress::Check() const
{
	if (export_name == nullptr || *export_name == 0)
		throw std::runtime_error("missing NFS_EXPORT");

	if (path == nullptr || *path == 0)
		throw std::runtime_error("missing NFS PATH");
}

bool
NfsAddress::IsValidBase() const
{
	return IsExpandable() || is_base(path);
}

NfsAddress *
NfsAddress::SaveBase(AllocatorPtr alloc,
		     std::string_view suffix) const noexcept
{
	const char *end = UriFindUnescapedSuffix(path, suffix);
	if (end == nullptr)
		return nullptr;

	auto dest = alloc.New<NfsAddress>(alloc.Dup(server),
					  alloc.Dup(export_name),
					  alloc.DupZ({path, end}));
	dest->content_type = alloc.CheckDup(content_type);
	return dest;
}

NfsAddress *
NfsAddress::LoadBase(AllocatorPtr alloc,
		     std::string_view suffix) const noexcept
{
	assert(path != nullptr);
	assert(*path != 0);
	assert(path[strlen(path) - 1] == '/');

	char *new_path = uri_unescape_concat(alloc, path, suffix);
	if (new_path == nullptr)
		return nullptr;

	auto dest = alloc.New<NfsAddress>(alloc.Dup(server),
					  alloc.Dup(export_name),
					  new_path);
	dest->content_type = alloc.CheckDup(content_type);
	return dest;
}

const NfsAddress *
NfsAddress::Expand(AllocatorPtr alloc, const MatchData &match_data) const
{
	if (!expand_path)
		return this;

	const char *new_path = expand_string_unescaped(alloc, path,
						       match_data);

	auto dest = alloc.New<NfsAddress>(server, export_name, new_path);
	dest->content_type = alloc.CheckDup(content_type);
	return dest;
}

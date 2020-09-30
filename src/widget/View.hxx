// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "ResourceAddress.hxx"
#include "bp/ForwardHeaders.hxx"
#include "util/IntrusiveForwardList.hxx"

struct Transformation;
class AllocatorPtr;

struct WidgetView {
	WidgetView *next = nullptr;

	/**
	 * The name of this view; always NULL for the first (default)
	 * view.
	 */
	const char *name = nullptr;

	/** the base URI of this widget, as specified in the template */
	ResourceAddress address{nullptr};

	/**
	 * Filter client error messages?
	 */
	bool filter_4xx = false;

	/**
	 * Copy of #TranslateResponse::subst_alt_syntax from
	 * #TranslationCommand::SUBST_ALT_SYNTAX.
	 */
	bool subst_alt_syntax = false;

	/**
	 * Was the address inherited from another view?
	 */
	bool inherited = false;

	IntrusiveForwardList<Transformation> transformations;

	/**
	 * Which request headers are forwarded?
	 */
	HeaderForwardSettings request_header_forward = HeaderForwardSettings::MakeDefaultRequest();

	/**
	 * Which response headers are forwarded?
	 */
	HeaderForwardSettings response_header_forward = HeaderForwardSettings::MakeDefaultResponse();

	explicit WidgetView(const char *_name) noexcept
		:name(_name) {}

	explicit constexpr WidgetView(const ResourceAddress &_address) noexcept
		:address(ShallowCopy(), _address) {}

	void CopyFrom(AllocatorPtr alloc, const WidgetView &src) noexcept;

	WidgetView *Clone(AllocatorPtr alloc) const noexcept;

	WidgetView *CloneChain(AllocatorPtr alloc) const noexcept;

	/**
	 * Copy the specified address into the view, if it does not have an
	 * address yet.
	 *
	 * @return true if the address was inherited, false if the view
	 * already had an address or if the specified address is empty
	 */
	bool InheritAddress(AllocatorPtr alloc,
			    const ResourceAddress &src) noexcept;

	/**
	 * Inherit the address and other related settings from one view to
	 * another.
	 *
	 * @return true if attributes were inherited, false if the destination
	 * view already had an address or if the source view's address is
	 * empty
	 */
	bool InheritFrom(AllocatorPtr alloc, const WidgetView &src) noexcept;

	/**
	 * Does the effective view enable the HTML processor?
	 */
	[[gnu::pure]]
	bool HasProcessor() const noexcept;

	/**
	 * Is this view a container?
	 */
	[[gnu::pure]]
	bool IsContainer() const noexcept;

	/**
	 * Does this view need to be expanded with widget_view_expand()?
	 */
	[[gnu::pure]]
	bool IsExpandable() const noexcept;

	/**
	 * Expand the strings in this view (not following the linked list)
	 * with the specified regex result.
	 *
	 * Throws std::runtime_error on error.
	 */
	void Expand(AllocatorPtr alloc, const MatchData &match_data) noexcept;
};

/**
 * Finds a view by its name.  If name==NULL, it returns the first
 * view.
 */
[[gnu::pure]]
const WidgetView *
widget_view_lookup(const WidgetView *view, const char *name) noexcept;

/**
 * Does any view in the linked list need to be expanded with
 * widget_view_expand()?
 */
[[gnu::pure]]
bool
widget_view_any_is_expandable(const WidgetView *view) noexcept;

/**
 * The same as widget_view_expand(), but expand all voews in
 * the linked list.
 */
void
widget_view_expand_all(AllocatorPtr alloc, WidgetView *view,
		       const MatchData &match_data) noexcept;

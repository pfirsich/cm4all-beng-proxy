// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "View.hxx"
#include "translation/Transformation.hxx"
#include "AllocatorPtr.hxx"

#include <string.h>

void
WidgetView::CopyFrom(AllocatorPtr alloc, const WidgetView &src) noexcept
{
	name = alloc.CheckDup(src.name);
	address.CopyFrom(alloc, src.address);
	filter_4xx = src.filter_4xx;
	inherited = src.inherited;
	transformations = Transformation::DupChain(alloc, src.transformations);
	request_header_forward = src.request_header_forward;
	response_header_forward = src.response_header_forward;
}

WidgetView *
WidgetView::Clone(AllocatorPtr alloc) const noexcept
{
	auto dest = alloc.New<WidgetView>(nullptr);
	dest->CopyFrom(alloc, *this);
	return dest;
}

WidgetView *
WidgetView::CloneChain(AllocatorPtr alloc) const noexcept
{
	assert(name == nullptr);

	WidgetView *dest = nullptr, **tail_p = &dest;

	for (const WidgetView *src = this; src != nullptr; src = src->next) {
		WidgetView *p = src->Clone(alloc);
		*tail_p = p;
		tail_p = &p->next;
	}

	return dest;

}

bool
WidgetView::InheritAddress(AllocatorPtr alloc,
			   const ResourceAddress &src) noexcept
{
	if (address.type != ResourceAddress::Type::NONE ||
	    src.type == ResourceAddress::Type::NONE)
		return false;

	address.CopyFrom(alloc, src);
	inherited = true;
	return true;
}

bool
WidgetView::InheritFrom(AllocatorPtr alloc, const WidgetView &src) noexcept
{
	if (InheritAddress(alloc, src.address)) {
		filter_4xx = src.filter_4xx;

		request_header_forward = src.request_header_forward;
		response_header_forward = src.response_header_forward;

		return true;
	} else
		return false;
}

const WidgetView *
widget_view_lookup(const WidgetView *view, const char *name) noexcept
{
	assert(view != nullptr);
	assert(view->name == nullptr);

	if (name == nullptr || *name == 0)
		/* the default view has no name */
		return view;

	for (view = view->next; view != nullptr; view = view->next) {
		assert(view->name != nullptr);

		if (strcmp(view->name, name) == 0)
			return view;
	}

	return nullptr;
}

bool
WidgetView::HasProcessor() const noexcept
{
	return Transformation::HasProcessor(transformations);
}

bool
WidgetView::IsContainer() const noexcept
{
	return Transformation::IsContainer(transformations);
}

bool
WidgetView::IsExpandable() const noexcept
{
	return address.IsExpandable() ||
		Transformation::IsChainExpandable(transformations);
}

bool
widget_view_any_is_expandable(const WidgetView *view) noexcept
{
	while (view != nullptr) {
		if (view->IsExpandable())
			return true;

		view = view->next;
	}

	return false;
}

void
WidgetView::Expand(AllocatorPtr alloc, const MatchData &match_data) noexcept
{
	address.Expand(alloc, match_data);
	Transformation::ExpandChain(alloc, transformations, match_data);
}

void
widget_view_expand_all(AllocatorPtr alloc, WidgetView *view,
		       const MatchData &match_data) noexcept
{
	while (view != nullptr) {
		view->Expand(alloc, match_data);
		view = view->next;
	}
}

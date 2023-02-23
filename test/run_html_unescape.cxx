// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "escape/HTML.hxx"
#include "escape/Static.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv) noexcept
{
	if (argc != 2)
		exit(1);

	const char *p = argv[1];
	const char *q = unescape_static(&html_escape_class, p);
	if (q == NULL) {
		fprintf(stderr, "too long\n");
		return EXIT_FAILURE;
	}

	printf("%s\n", q);
}

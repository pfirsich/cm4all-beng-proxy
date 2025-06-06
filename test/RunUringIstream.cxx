// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "TestInstance.hxx"
#include "istream/UringIstream.hxx"
#include "istream/sink_fd.hxx"
#include "istream/UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "system/Error.hxx"
#include "io/Open.hxx"
#include "io/SharedFd.hxx"
#include "io/SpliceSupport.hxx"
#include "io/uring/Handler.hxx"
#include "io/uring/OpenStat.hxx"
#include "util/PrintException.hxx"

#include <liburing.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

struct UringInstance : TestInstance {
	UringInstance() {
		event_loop.EnableUring(1024, IORING_SETUP_SINGLE_ISSUER|IORING_SETUP_COOP_TASKRUN);
	}
};

struct Context final : UringInstance, Uring::OpenStatHandler, SinkFdHandler {
	Uring::OpenStat open_stat;

	const char *_path;

	SinkFd *sink = nullptr;
	std::exception_ptr error;

	Context()
		:open_stat(*event_loop.GetUring(), *this) {}

	void BeginShutdown() noexcept {
		event_loop.SetVolatile();
	}

	void Open(const char *path) noexcept;

	void CreateSinkFd(const char *path,
			  UniqueFileDescriptor &&fd,
			  off_t size) noexcept;

	/* virtual methods from class Uring::OpenStatHandler */
	void OnOpenStat(UniqueFileDescriptor fd,
			struct statx &st) noexcept override;
	void OnOpenStatError(int error) noexcept override;

	/* virtual methods from class SinkFdHandler */
	void OnInputEof() noexcept override;
	void OnInputError(std::exception_ptr ep) noexcept override;
	bool OnSendError(int error) noexcept override;
};

inline void
Context::Open(const char *path) noexcept
{
	_path = path;
	open_stat.StartOpenStatReadOnly(path);
}

void
Context::CreateSinkFd(const char *path, UniqueFileDescriptor &&fd,
		      off_t size) noexcept
{
	auto *shared_fd = NewFromPool<SharedFd>(root_pool, std::move(fd));

	sink = sink_fd_new(event_loop, root_pool,
			   NewUringIstream(*event_loop.GetUring(),
					   root_pool, path,
					   shared_fd->Get(), *shared_fd,
					   0, size),
			   FileDescriptor(STDOUT_FILENO),
			   guess_fd_type(STDOUT_FILENO),
			   *this);
}

void
Context::OnOpenStat(UniqueFileDescriptor fd,
		    struct statx &st) noexcept
try {
	if (!S_ISREG(st.stx_mode))
		throw std::runtime_error("Not a regular file");

	CreateSinkFd(_path, std::move(fd), st.stx_size);
} catch (...) {
	error = std::current_exception();
	BeginShutdown();
}

void
Context::OnOpenStatError(int _error) noexcept
{
	error = std::make_exception_ptr(MakeErrno(_error, "Failed to open file"));
	BeginShutdown();
}

void
Context::OnInputEof() noexcept
{
	sink = nullptr;
	BeginShutdown();
}

void
Context::OnInputError(std::exception_ptr ep) noexcept
{
	sink = nullptr;
	error = std::move(ep);
	BeginShutdown();
}

bool
Context::OnSendError(int _error) noexcept
{
	sink = nullptr;
	error = std::make_exception_ptr(MakeErrno(_error, "Failed to write"));
	BeginShutdown();

	return true;
}

int
main(int argc, char **argv)
try {
	if (argc != 2) {
		fprintf(stderr, "Usage: %s PATH\n", argv[0]);
		return EXIT_FAILURE;
	}

	const char *path = argv[1];

	Context context;
	context.Open(path);

	context.event_loop.Run();

	if (context.error)
		std::rethrow_exception(context.error);

	return EXIT_SUCCESS;
} catch (...) {
	PrintException(std::current_exception());
	return EXIT_FAILURE;
}

/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

typedef DIR *(*OpendirFunction)(const char *);
typedef DIR *(*FdopendirFunction)(int);

static OpendirFunction next_opendir;
static FdopendirFunction next_fdopendir;
static atomic_uint_fast64_t directory_enumerations;
static atomic_bool counting;

__attribute__((constructor)) static void resolve_directory_functions(void) {
	next_opendir = (OpendirFunction)dlsym(RTLD_NEXT, "opendir");
	next_fdopendir = (FdopendirFunction)dlsym(RTLD_NEXT, "fdopendir");
}

DIR *opendir(const char *name) {
	if (!next_opendir) {
		errno = ENOSYS;
		return NULL;
	}
	DIR *result = next_opendir(name);
	if (result && atomic_load_explicit(&counting, memory_order_relaxed)) {
		atomic_fetch_add_explicit(
			&directory_enumerations, 1, memory_order_relaxed);
	}
	return result;
}

DIR *fdopendir(int fd) {
	if (!next_fdopendir) {
		errno = ENOSYS;
		return NULL;
	}
	DIR *result = next_fdopendir(fd);
	if (result && atomic_load_explicit(&counting, memory_order_relaxed)) {
		atomic_fetch_add_explicit(
			&directory_enumerations, 1, memory_order_relaxed);
	}
	return result;
}

void DesktopScaleCheckStartDirectoryCount(void) {
	atomic_store_explicit(&directory_enumerations, 0, memory_order_relaxed);
	atomic_store_explicit(&counting, true, memory_order_relaxed);
}

uint64_t DesktopScaleCheckStopDirectoryCount(void) {
	atomic_store_explicit(&counting, false, memory_order_relaxed);
	return atomic_load_explicit(&directory_enumerations, memory_order_relaxed);
}

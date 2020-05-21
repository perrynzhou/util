//
// Created by hujianzhe
//

#include "../../inc/sysapi/error.h"
#include "../../inc/sysapi/file.h"
#include "../../inc/sysapi/misc.h"
#include "../../inc/sysapi/time.h"
#include "../../inc/component/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct CacheBlock_t {
	ListNode_t m_listnode;
	struct tm dt;
	size_t len;
	char txt[1];
} CacheBlock_t;

#ifdef	__cplusplus
extern "C" {
#endif

static void log_rotate(Log_t* log, const struct tm* dt) {
	FD_t fd;
	char* pathname;
	long long filesz;
	while (1) {
		pathname = strFormat(NULL, "%s.%d-%d-%d.%u.txt", log->pathname, dt->tm_year, dt->tm_mon, dt->tm_mday, log->m_filesegmentseq);
		if (!pathname)
			return;
		fd = fdOpen(pathname, FILE_READ_BIT);
		if (fd != INVALID_FD_HANDLE) {
			filesz = fdGetSize(fd);
			fdClose(fd);
			if (filesz < 0) {
				free(pathname);
				return;
			}
		}
		else if (errnoGet() == ENOENT) {
			filesz = 0;
		}
		else {
			free(pathname);
			return;
		}
		log->m_filesegmentseq++;
		if (filesz < log->m_maxfilesize)
			break;
		free(pathname);
	}
	fd = fdOpen(pathname, FILE_CREAT_BIT | FILE_WRITE_BIT | FILE_APPEND_BIT);
	free(pathname);
	if (INVALID_FD_HANDLE == fd)
		return;
	if (log->m_fd != INVALID_FD_HANDLE) {
		fdClose(log->m_fd);
	}
	log->m_fd = fd;
	log->m_filesize = filesz;
}

static void log_write(Log_t* log, CacheBlock_t* cache) {
	unsigned char is_async = log->async_print_file;
	if (log->print_stderr) {
		fputs(cache->txt, stderr);
	}
	if (log->print_file) {
		struct tm* dt = &cache->dt;

		criticalsectionEnter(&log->m_lock);

		if (is_async) {
			listInsertNodeBack(&log->m_cachelist, log->m_cachelist.tail, &cache->m_listnode);
		}
		else {
			/* day rotate */
			if (log->m_days != dt->tm_yday) {
				log->m_days = dt->tm_yday;
				log->m_filesegmentseq = 0;
				log_rotate(log, dt);
			}
			/* size rotate */
			else if (log->m_filesize + cache->len >= log->m_maxfilesize) {
				log_rotate(log, dt);
			}
			/* io */
			if (INVALID_FD_HANDLE != log->m_fd) {
				fdWrite(log->m_fd, cache->txt, cache->len);
			}
			log->m_filesize += cache->len;
		}

		criticalsectionLeave(&log->m_lock);
	}
	if (!is_async)
		free(cache);
}

#define log_build(log, priority, format) do {\
	va_list varg;\
	int len, res;\
	char test_buf;\
	CacheBlock_t* cache;\
	struct tm dt;\
\
	if (!format || 0 == *format) {\
		return;\
	}\
	if (!structtmMake(gmtimeSecond(), &dt)) {\
		return;\
	}\
	structtmNormal(&dt);\
	res = strFormatLen("%s|%d-%d-%d %d:%d:%d|%s|",\
						log->ident,\
						dt.tm_year, dt.tm_mon, dt.tm_mday,\
						dt.tm_hour, dt.tm_min, dt.tm_sec,\
						priority);\
	if (res <= 0)\
		return;\
	len = res;\
	va_start(varg, format);\
	res = vsnprintf(&test_buf, 0, format, varg);\
	va_end(varg);\
	if (res <= 0)\
		return;\
	len += res;\
\
	cache = (CacheBlock_t*)malloc(sizeof(CacheBlock_t) + len + 1);\
	if (!cache)\
		return;\
	cache->dt = dt;\
	cache->len = len;\
\
	res = snprintf(cache->txt, cache->len, "%s|%d-%d-%d %d:%d:%d|%s|",\
					log->ident,\
					dt.tm_year, dt.tm_mon, dt.tm_mday,\
					dt.tm_hour, dt.tm_min, dt.tm_sec,\
					priority);\
	if (res <= 0 || res >= cache->len) {\
		free(cache);\
		return;\
	}\
	va_start(varg, format);\
	res = vsnprintf(cache->txt + res, cache->len - res + 1, format, varg);\
	va_end(varg);\
	if (res <= 0) {\
		free(cache);\
		return;\
	}\
	cache->txt[cache->len] = 0;\
\
	log_write(log, cache);\
} while (0)

Log_t* logInit(Log_t* log, const char ident[64], const char* pathname) {
	log->m_initok = 0;
	log->pathname = strdup(pathname);
	if (!log->pathname)
		return NULL;
	if (!criticalsectionCreate(&log->m_lock)) {
		free(log->pathname);
		return NULL;
	}
	log->m_days = -1;
	log->m_fd = INVALID_FD_HANDLE;
	log->m_filesize = 0;
	log->m_maxfilesize = ~0;
	log->m_filesegmentseq = 0;
	listInit(&log->m_cachelist);
	log->m_initok = 1;

	strncpy(log->ident, ident, sizeof(log->ident) - 1);
	log->ident[sizeof(log->ident) - 1] = 0;
	log->print_stderr = 0;
	log->print_file = 1;
	log->async_print_file = 0;

	return log;
}

void logFlush(Log_t* log) {
	char *txt = NULL;
	size_t txtlen = 0;
	ListNode_t *cur, *next;

	criticalsectionEnter(&log->m_lock);

	cur = log->m_cachelist.head;
	listInit(&log->m_cachelist);

	criticalsectionLeave(&log->m_lock);

	for (; cur; cur = next) {
		char *p;
		CacheBlock_t* cache = pod_container_of(cur, CacheBlock_t, m_listnode);
		next = cur->next;
		/* day rotate */
		if (log->m_days != cache->dt.tm_yday) {
			log->m_days = cache->dt.tm_yday;
			log->m_filesegmentseq = 0;
			if (txt && log->m_fd != INVALID_FD_HANDLE) {
				fdWrite(log->m_fd, txt, txtlen);
			}
			free(txt);
			txt = NULL;
			txtlen = 0;
			log_rotate(log, &cache->dt);
		}
		/* size rotate */
		else if (cache->len >= log->m_maxfilesize - log->m_filesize) {
			free(txt);
			txt = NULL;
			txtlen = 0;
			log_rotate(log, &cache->dt);
		}
		/* copy data to cache */
		p = (char*)realloc(txt, txtlen + cache->len);
		if (p) {
			txt = p;
			memcpy(txt + txtlen, cache->txt, cache->len);
			txtlen += cache->len;
			log->m_filesize += cache->len;
		}
		else {
			log->m_filesize -= txtlen;
			free(txt);
			txt = NULL;
			txtlen = 0;
		}
		free(cache);
	}
	/* io */
	if (log->m_fd != INVALID_FD_HANDLE && txt) {
		fdWrite(log->m_fd, txt, txtlen);
	}
	free(txt);
}

void logClear(Log_t* log) {
	ListNode_t *cur, *next;

	criticalsectionEnter(&log->m_lock);

	cur = log->m_cachelist.head;
	listInit(&log->m_cachelist);

	criticalsectionLeave(&log->m_lock);

	for (; cur; cur = next) {
		next = cur->next;
		free(pod_container_of(cur, CacheBlock_t, m_listnode));
	}
}

void logDestroy(Log_t* log) {
	if (log && log->m_initok) {
		logClear(log);
		criticalsectionClose(&log->m_lock);
		free(log->pathname);
		if (INVALID_FD_HANDLE != log->m_fd)
			fdClose(log->m_fd);
	}
}

void logEmerg(Log_t* log, const char* format, ...) {
	log_build(log, "EMERG", format);
}
void logAlert(Log_t* log, const char* format, ...) {
	log_build(log, "ALERT", format);
}
void logCrit(Log_t* log, const char* format, ...) {
	log_build(log, "CRIT", format);
}
void logErr(Log_t* log, const char* format, ...) {
	log_build(log, "ERR", format);
}
void logWarning(Log_t* log, const char* format, ...) {
	log_build(log, "WARNING", format);
}
void logNotice(Log_t* log, const char* format, ...) {
	log_build(log, "NOTICE", format);
}
void logInfo(Log_t* log, const char* format, ...) {
	log_build(log, "INFO", format);
}
void logDebug(Log_t* log, const char* format, ...) {
	log_build(log, "DEBUG", format);
}

#ifdef	__cplusplus
}
#endif

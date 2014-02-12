/*
 * Author: Landon Fuller <landonf@plausible.coop>
 *
 * Copyright (c) 2008-2013 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#import "PLCrashAsync.h"

#import <stdint.h>
#import <errno.h>
#import <string.h>
#import <inttypes.h>

/**
 * @internal
 * @defgroup plcrash_async Async Safe Utilities
 * @ingroup plcrash_internal
 *
 * Implements async-safe utility functions
 *
 * @{
 */

/**
 * Return an error description for the given plcrash_error_t.
 */
const char *plcrash_async_strerror (plcrash_error_t error) {
    switch (error) {
        case PLCRASH_ESUCCESS:
            return "No error";
        case PLCRASH_EUNKNOWN:
            return "Unknown error";
        case PLCRASH_OUTPUT_ERR:
            return "Output file can not be opened (or written to)";
        case PLCRASH_ENOMEM:
            return "No memory available";
        case PLCRASH_ENOTSUP:
            return "Operation not supported";
        case PLCRASH_EINVAL:
            return "Invalid argument";
        case PLCRASH_EINTERNAL:
            return "Internal error";
        case PLCRASH_EACCESS:
            return "Access denied";
        case PLCRASH_ENOTFOUND:
            return "Not found";
    }
    
    /* Should be unreachable */
    return "Unhandled error code";
}

/**
 * (Safely) read len bytes from @a source, storing in @a dest.
 *
 * @param task The task from which data from address @a source will be read.
 * @param source The address within @a task from which the data will be read.
 * @param dest The destination address to which copied data will be written.
 * @param len The number of bytes to be read.
 *
 * @return On success, returns KERN_SUCCESS. If the pages containing @a source + len are unmapped, KERN_INVALID_ADDRESS
 * will be returned. If the pages can not be read due to access restrictions, KERN_PROTECTION_FAILURE will be returned.
 *
 * @warning Unlike all other plcrash_* functions, plcrash_async_read_addr returns a kern_return_t value.
 * @todo Modify plcrash_async_read_addr and all API clients to use plcrash_error_t values.
 */
kern_return_t plcrash_async_read_addr (mach_port_t task, pl_vm_address_t source, void *dest, pl_vm_size_t len) {
#ifdef PL_HAVE_MACH_VM
    pl_vm_size_t read_size = len;
    return mach_vm_read_overwrite(task, source, len, (pointer_t) dest, &read_size);
#else
    vm_size_t read_size = len;
    return vm_read_overwrite(task, source, len, (pointer_t) dest, &read_size);
#endif
}

/**
 * An intentionally naive async-safe implementation of strcmp(). strcmp() itself is not declared to be async-safe,
 * though in reality, it is.
 *
 * @param s1 First string.
 * @param s2 Second string.
 * @return Return an integer greater than, equal to, or less than 0, according as the string @a s1 is greater than,
 * equal to, or less than the string @a s2.
 */
int plcrash_async_strncmp(const char *s1, const char *s2, size_t n) {
    while (*s1 == *s2++ && n-- > 0) {
        if (*s1++ == 0)
            return (0);
    }
    
    if (n == 0)
        return 0;

    return (*(const unsigned char *)s1 - *(const unsigned char *)(s2 - 1));
}

/**
 * An intentionally naive async-safe implementation of memcpy(). memcpy() itself is not declared to be async-safe.
 *
 * @param dest Destination.
 * @param source Source.
 * @param n Number of bytes to copy.
 */
void *plcrash_async_memcpy (void *dest, const void *source, size_t n) {
    uint8_t *s = (uint8_t *) source;
    uint8_t *d = (uint8_t *) dest;

    for (size_t count = 0; count < n; count++)
        *d++ = *s++;

    return (void *) source;
}

/**
 * @internal
 * @ingroup plcrash_async
 * @defgroup plcrash_async_bufio Async-safe Buffered IO
 * @{
 */

/**
 * 
 * Write len bytes to fd, looping until all bytes are written
 * or an error occurs. For the local file system, only one call to write()
 * should be necessary
 */
ssize_t plcrash_async_writen (int fd, const void *data, size_t len) {
    const void *p;
    size_t left;
    ssize_t written = 0;
    
    /* Loop until all bytes are written */
    p = data;
    left = len;
    while (left > 0) {
        if ((written = write(fd, p, left)) <= 0) {
            if (errno == EINTR) {
                // Try again
                written = 0;
            } else {
                return -1;
            }
        }
        
        left -= written;
        p += written;
    }
    
    return written;
}


/**
 * Initialize the plcrash_async_file_t instance.
 *
 * @param file File structure to initialize.
 * @param output_limit Maximum number of bytes that will be written to disk. Intended as a
 * safety measure prevent a run-away crash log writer from filling the disk. Specify
 * 0 to disable any limits. Once the limit is reached, all data will be dropped.
 * @param fd Open file descriptor.
 */
void plcrash_async_file_init (plcrash_async_file_t *file, int fd, off_t output_limit) {
    file->fd = fd;
    file->buflen = 0;
    file->total_bytes = 0;
    file->limit_bytes = output_limit;
}


/**
 * Write all bytes from @a data to the file buffer. Returns true on success,
 * or false if an error occurs.
 */
bool plcrash_async_file_write (plcrash_async_file_t *file, const void *data, size_t len) {
    /* Check and update output limit */
    if (file->limit_bytes != 0 && len + file->total_bytes > file->limit_bytes) {
        return false;
    } else if (file->limit_bytes != 0) {
        file->total_bytes += len;
    }

    /* Check if the buffer will fill */
    if (file->buflen + len > sizeof(file->buffer)) {
        /* Flush the buffer */
        if (plcrash_async_writen(file->fd, file->buffer, file->buflen) < 0) {
            PLCF_DEBUG("Error occured writing to crash log: %s", strerror(errno));
            return false;
        }
        
        file->buflen = 0;
    }
    
    /* Check if the new data fits within the buffer, if so, buffer it */
    if (len + file->buflen <= sizeof(file->buffer)) {
        plcrash_async_memcpy(file->buffer + file->buflen, data, len);
        file->buflen += len;
        
        return true;
        
    } else {
        /* Won't fit in the buffer, just write it */
        if (plcrash_async_writen(file->fd, data, len) < 0) {
            PLCF_DEBUG("Error occured writing to crash log: %s", strerror(errno));
            return false;
        }
        
        return true;
    } 
}


/**
 * Flush all buffered bytes from the file buffer.
 */
bool plcrash_async_file_flush (plcrash_async_file_t *file) {
    /* Anything to do? */
    if (file->buflen == 0)
        return true;
    
    /* Write remaining */
    if (plcrash_async_writen(file->fd, file->buffer, file->buflen) < 0) {
        PLCF_DEBUG("Error occured writing to crash log: %s", strerror(errno));
        return false;
    }
    
    file->buflen = 0;
    
    return true;
}


/**
 * Close the backing file descriptor.
 */
bool plcrash_async_file_close (plcrash_async_file_t *file) {
    /* Flush any pending data */
    if (!plcrash_async_file_flush(file))
        return false;

    /* Close the file descriptor */
    if (close(file->fd) != 0) {
        PLCF_DEBUG("Error closing file: %s", strerror(errno));
        return false;
    }

    return true;
}

/**
 * @} plcrash_async_bufio
 */

/**
 * @} plcrash_async
 */
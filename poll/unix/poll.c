/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2002 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 */

#include "apr.h"
#include "apr_poll.h"
#include "apr_time.h"
#include "networkio.h"
#include "fileio.h"
#if HAVE_POLL_H
#include <poll.h>
#endif
#if HAVE_SYS_POLL_H
#include <sys/poll.h>
#endif

#ifdef HAVE_POLL    /* We can just use poll to do our socket polling. */

static apr_int16_t get_event(apr_int16_t event)
{
    apr_int16_t rv = 0;

    if (event & APR_POLLIN)
        rv |= POLLIN;        
    if (event & APR_POLLPRI)
        rv |= POLLPRI;        
    if (event & APR_POLLOUT)
        rv |= POLLOUT;       
    if (event & APR_POLLERR)
        rv |= POLLERR;        
    if (event & APR_POLLHUP)
        rv |= POLLHUP;        
    if (event & APR_POLLNVAL)
        rv |= POLLNVAL;        

    return rv;
}

static apr_int16_t get_revent(apr_int16_t event)
{
    apr_int16_t rv = 0;

    if (event & POLLIN)
        rv |= APR_POLLIN;
    if (event & POLLPRI)
        rv |= APR_POLLPRI;
    if (event & POLLOUT)
        rv |= APR_POLLOUT;
    if (event & POLLERR)
        rv |= APR_POLLERR;
    if (event & POLLHUP)
        rv |= APR_POLLHUP;
    if (event & POLLNVAL)
        rv |= APR_POLLNVAL;

    return rv;
}        

#define SMALL_POLLSET_LIMIT  8

APR_DECLARE(apr_status_t) apr_poll(apr_pollfd_t *aprset, apr_int32_t num,
                      apr_int32_t *nsds, apr_interval_time_t timeout)
{
    struct pollfd tmp_pollset[SMALL_POLLSET_LIMIT];
    struct pollfd *pollset;
    int i;

    if (num <= SMALL_POLLSET_LIMIT) {
        pollset = tmp_pollset;
    }
    else {
        /* XXX There are two problems with this code: it leaks
         * memory, and it requires an O(n)-time loop to copy
         * n descriptors from the apr_pollfd_t structs into
         * the pollfd structs.  At the moment, it's best suited
         * for use with fewer than SMALL_POLLSET_LIMIT
         * descriptors.
         */
        pollset = apr_palloc(aprset->p,
                             sizeof(struct pollfd) * num);
    }
    for (i = 0; i < num; i++) {
        if (aprset[i].desc_type == APR_POLL_SOCKET) {
            pollset[i].fd = aprset[i].desc.s->socketdes;
        }
        else if (aprset[i].desc_type == APR_POLL_FILE) {
            pollset[i].fd = aprset[i].desc.f->filedes;
        }
        pollset[i].events = get_event(aprset[i].reqevents);
    }

    if (timeout > 0) {
        timeout /= 1000; /* convert microseconds to milliseconds */
    }

    i = poll(pollset, num, timeout);
    (*nsds) = i;

    for (i = 0; i < num; i++) {
        aprset[i].rtnevents = get_revent(pollset[i].revents);
    }
    
    if ((*nsds) < 0) {
        return errno;
    }
    if ((*nsds) == 0) {
        return APR_TIMEUP;
    }
    return APR_SUCCESS;
}


#else    /* Use select to mimic poll */

APR_DECLARE(apr_status_t) apr_poll(apr_pollfd_t *aprset, int num, apr_int32_t *nsds, 
		    apr_interval_time_t timeout)
{
    fd_set readset, writeset, exceptset;
    int rv, i;
    int maxfd = -1;
    struct timeval tv, *tvptr;
#ifdef NETWARE
    int is_pipe = 0;
#endif

    if (timeout < 0) {
        tvptr = NULL;
    }
    else {
        tv.tv_sec = (long)apr_time_sec(timeout);
        tv.tv_usec = (long)apr_time_usec(timeout);
        tvptr = &tv;
    }

    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&exceptset);

    for (i = 0; i < num; i++) {
        int fd;

        if (aprset[i].desc_type == APR_POLL_SOCKET) {
            fd = aprset[i].desc.s->socketdes;
        }
        else if (aprset[i].desc_type == APR_POLL_FILE) {
            fd = aprset[i].desc.f->filedes;
#ifdef NETWARE
            is_pipe = aprset[i].desc.f->is_pipe;
#endif
        }
        if (aprset[i].reqevents & APR_POLLIN) {
            FD_SET(fd, &readset);
        }
        if (aprset[i].reqevents & APR_POLLOUT) {
            FD_SET(fd, &writeset);
        }
        if (aprset[i].reqevents & 
            (APR_POLLPRI | APR_POLLERR | APR_POLLHUP | APR_POLLNVAL)) {
            FD_SET(fd, &exceptset);
        }
        if (fd > maxfd) {
            maxfd = fd;
        }
    }

#ifdef NETWARE
    if (is_pipe) {
        rv = pipe_select(maxfd + 1, &readset, &writeset, &exceptset, tvptr);
    }
    else {
#endif

    rv = select(maxfd + 1, &readset, &writeset, &exceptset, tvptr);

#ifdef NETWARE
    }
#endif

    (*nsds) = rv;
    if ((*nsds) == 0) {
        return APR_TIMEUP;
    }
    if ((*nsds) < 0) {
        return errno;
    }

    for (i = 0; i < num; i++) {
        int fd;

        if (aprset[i].desc_type == APR_POLL_SOCKET) {
            fd = aprset[i].desc.s->socketdes;
        }
        else if (aprset[i].desc_type == APR_POLL_FILE) {
            fd = aprset[i].desc.f->filedes;
        }
        aprset[i].rtnevents = 0;
        if (FD_ISSET(fd, &readset)) {
            aprset[i].rtnevents |= APR_POLLIN;
        }
        if (FD_ISSET(fd, &writeset)) {
            aprset[i].rtnevents |= APR_POLLOUT;
        }
        if (FD_ISSET(fd, &exceptset)) {
            aprset[i].rtnevents |= APR_POLLERR;
        }
    }

    return APR_SUCCESS;
}

#endif 


struct apr_pollset_t {
    apr_uint32_t nelts;
    apr_uint32_t nalloc;
    struct pollfd *pollset;
    apr_pollfd_t *query_set;
    apr_pollfd_t *result_set;
    apr_pool_t *pool;
};

APR_DECLARE(apr_status_t) apr_pollset_create(apr_pollset_t **pollset,
                                             apr_uint32_t size,
                                             apr_pool_t *p)
{
    *pollset = apr_palloc(p, sizeof(**pollset));
    (*pollset)->nelts = 0;
    (*pollset)->nalloc = size;
    (*pollset)->pollset = apr_palloc(p, size * sizeof(struct pollfd));
    (*pollset)->query_set = apr_palloc(p, size * sizeof(apr_pollfd_t));
    (*pollset)->result_set = apr_palloc(p, size * sizeof(apr_pollfd_t));
    (*pollset)->pool = p;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollset_destroy(apr_pollset_t *pollset)
{
    /* A no-op function for now.  If we later implement /dev/poll
     * support, we'll need to close the /dev/poll fd here
     */
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollset_add(apr_pollset_t *pollset,
                                          const apr_pollfd_t *descriptor)
{
    if (pollset->nelts == pollset->nalloc) {
        return APR_ENOMEM;
    }

    pollset->query_set[pollset->nelts] = *descriptor;
    if (descriptor->desc_type == APR_POLL_SOCKET) {
        pollset->pollset[pollset->nelts].fd = descriptor->desc.s->socketdes;
    }
    else {
        pollset->pollset[pollset->nelts].fd = descriptor->desc.f->filedes;
    }
    pollset->pollset[pollset->nelts].events = get_event(descriptor->reqevents);
    pollset->nelts++;
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollset_remove(apr_pollset_t *pollset,
                                             const apr_pollfd_t *descriptor)
{
    apr_uint32_t i;
    int fd;

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }

    for (i = 0; i < pollset->nelts; i++) {
        if (fd == pollset->pollset[i].fd) {
            /* Found an instance of the fd: remove this and any other copies */
            apr_uint32_t dst = i;
            apr_uint32_t old_nelts = pollset->nelts;
            pollset->nelts--;
            for (i++; i < old_nelts; i++) {
                if (fd == pollset->pollset[i].fd) {
                    pollset->nelts--;
                }
                else {
                    pollset->pollset[dst] = pollset->pollset[i];
                }
            }
            return APR_SUCCESS;
        }
    }
    return APR_NOTFOUND;
}

APR_DECLARE(apr_status_t) apr_pollset_poll(apr_pollset_t *pollset,
                                           apr_interval_time_t timeout,
                                           apr_int32_t *num,
                                           const apr_pollfd_t **descriptors)
{
    int rv;
    apr_uint32_t i, j;

    if (timeout > 0) {
        timeout /= 1000;
    }
    rv = poll(pollset->pollset, pollset->nelts, timeout);
    (*num) = rv;
    if (rv < 0) {
        return errno;
    }
    j = 0;
    for (i = 0; i < pollset->nelts; i++) {
        if (pollset->pollset[i].revents != 0) {
            pollset->result_set[j] = pollset->query_set[i];
            pollset->result_set[j].rtnevents =
                get_revent(pollset->pollset[i].revents);
            j++;
        }
    }
    *descriptors = pollset->result_set;
    return APR_SUCCESS;
}

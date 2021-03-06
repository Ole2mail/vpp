/*
 * Copyright (c) 2016 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vnet/fib/fib_entry_delegate.h>
#include <vnet/fib/fib_entry.h>

static fib_entry_delegate_t *
fib_entry_delegate_find_i (const fib_entry_t *fib_entry,
                           fib_entry_delegate_type_t type,
                           u32 *index)
{
    fib_entry_delegate_t *delegate;
    int ii;

    ii = 0;
    vec_foreach(delegate, fib_entry->fe_delegates)
    {
	if (delegate->fd_type == type)
	{
            if (NULL != index)
                *index = ii;

	    return (delegate);
	}
	else
	{
	    ii++;
	}
    }

    return (NULL);
}

fib_entry_delegate_t *
fib_entry_delegate_get (const fib_entry_t *fib_entry,
                        fib_entry_delegate_type_t type)
{
    return (fib_entry_delegate_find_i(fib_entry, type, NULL));
}

void
fib_entry_delegate_remove (fib_entry_t *fib_entry,
                           fib_entry_delegate_type_t type)
{
    fib_entry_delegate_t *fed;
    u32 index = ~0;

    fed = fib_entry_delegate_find_i(fib_entry, type, &index);

    ASSERT(NULL != fed);

    vec_del1(fib_entry->fe_delegates, index);
}

static int
fib_entry_delegate_cmp_for_sort (void * v1,
                                 void * v2)
{
    fib_entry_delegate_t *delegate1 = v1, *delegate2 = v2;

    return (delegate1->fd_type - delegate2->fd_type);
}

static void
fib_entry_delegate_init (fib_entry_t *fib_entry,
                         fib_entry_delegate_type_t type)

{
    fib_entry_delegate_t delegate = {
	.fd_entry_index = fib_entry_get_index(fib_entry),
	.fd_type = type,
    };

    vec_add1(fib_entry->fe_delegates, delegate);
    vec_sort_with_function(fib_entry->fe_delegates,
			   fib_entry_delegate_cmp_for_sort);
}

fib_entry_delegate_t *
fib_entry_delegate_find_or_add (fib_entry_t *fib_entry,
                                fib_entry_delegate_type_t fdt)
{
    fib_entry_delegate_t *delegate;

    delegate = fib_entry_delegate_get(fib_entry, fdt);

    if (NULL == delegate)
    {
	fib_entry_delegate_init(fib_entry, fdt);
    }

    return (fib_entry_delegate_get(fib_entry, fdt));
}

fib_entry_delegate_type_t
fib_entry_chain_type_to_delegate_type (fib_forward_chain_type_t fct)
{
    switch (fct)
    {
    case FIB_FORW_CHAIN_TYPE_UNICAST_IP4:
        return (FIB_ENTRY_DELEGATE_CHAIN_UNICAST_IP4);
    case FIB_FORW_CHAIN_TYPE_UNICAST_IP6:
        return (FIB_ENTRY_DELEGATE_CHAIN_UNICAST_IP6);
    case FIB_FORW_CHAIN_TYPE_MPLS_EOS:
        return (FIB_ENTRY_DELEGATE_CHAIN_MPLS_EOS);
    case FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS:
        return (FIB_ENTRY_DELEGATE_CHAIN_MPLS_NON_EOS);
    case FIB_FORW_CHAIN_TYPE_ETHERNET:
        return (FIB_ENTRY_DELEGATE_CHAIN_ETHERNET);
    case FIB_FORW_CHAIN_TYPE_MCAST_IP4:
    case FIB_FORW_CHAIN_TYPE_MCAST_IP6:
        break;
    case FIB_FORW_CHAIN_TYPE_NSH:
        return (FIB_ENTRY_DELEGATE_CHAIN_NSH);
    }
    ASSERT(0);
    return (FIB_ENTRY_DELEGATE_CHAIN_UNICAST_IP4);
}

fib_forward_chain_type_t
fib_entry_delegate_type_to_chain_type (fib_entry_delegate_type_t fdt)
{
    switch (fdt)
    {
    case FIB_ENTRY_DELEGATE_CHAIN_UNICAST_IP4:
        return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
    case FIB_ENTRY_DELEGATE_CHAIN_UNICAST_IP6:
        return (FIB_FORW_CHAIN_TYPE_UNICAST_IP6);
    case FIB_ENTRY_DELEGATE_CHAIN_MPLS_EOS:
        return (FIB_FORW_CHAIN_TYPE_MPLS_EOS);
    case FIB_ENTRY_DELEGATE_CHAIN_MPLS_NON_EOS:
        return (FIB_FORW_CHAIN_TYPE_MPLS_NON_EOS);
    case FIB_ENTRY_DELEGATE_CHAIN_ETHERNET:
        return (FIB_FORW_CHAIN_TYPE_ETHERNET);
    case FIB_ENTRY_DELEGATE_CHAIN_NSH:
        return (FIB_FORW_CHAIN_TYPE_NSH);
    case FIB_ENTRY_DELEGATE_COVERED:
    case FIB_ENTRY_DELEGATE_ATTACHED_IMPORT:
    case FIB_ENTRY_DELEGATE_ATTACHED_EXPORT:
        break;
    }
    ASSERT(0);
    return (FIB_FORW_CHAIN_TYPE_UNICAST_IP4);
}

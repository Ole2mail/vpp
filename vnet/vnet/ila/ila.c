#include <vnet/ila/ila.h>

static ila_main_t ila_main;

#define ILA_TABLE_DEFAULT_HASH_NUM_BUCKETS (64 * 1024)
#define ILA_TABLE_DEFAULT_HASH_MEMORY_SIZE (32<<20)

#define foreach_ila_error                               \
 _(NONE, "valid ILA packets")

typedef enum {
#define _(sym,str) ILA_ERROR_##sym,
   foreach_ila_error
#undef _
   ILA_N_ERROR,
 } ila_error_t;

 static char *ila_error_strings[] = {
 #define _(sym,string) string,
   foreach_ila_error
 #undef _
 };

typedef enum {
  ILA_ILA2SIR_NEXT_IP6_REWRITE,
  ILA_ILA2SIR_NEXT_DROP,
  ILA_ILA2SIR_N_NEXT,
} ila_ila2sir_next_t;

typedef struct {
  u32 ila_index;
  ip6_address_t initial_dst;
} ila_ila2sir_trace_t;

static u8 *
format_half_ip6_address(u8 *s, va_list *va)
{
  u64 v = clib_net_to_host_u64(va_arg(*va, u64));

  return format(s, "%04x:%04x:%04x:%04x",
    v >> 48,
    (v >> 32) & 0xffff,
    (v >> 16) & 0xffff,
    v & 0xffff);

}

static u8 *
format_csum_mode(u8 *s, va_list *va)
{
  ila_csum_mode_t csum_mode = va_arg(*va, ila_csum_mode_t);
  const char *txt;

  switch(csum_mode) {
    case ILA_CSUM_MODE_NO_ACTION:
      txt = "no-action";
      break;
    case ILA_CSUM_MODE_NEUTRAL_MAP:
      txt = "neutral-map";
      break;
    case ILA_CSUM_MODE_ADJUST_TRANSPORT:
      txt = "transport-adjust";
      break;
    default:
      txt = "(unknown)";
      break;
  }
  return format(s, "%s", txt);
}

static u8 *
format_ila_entry(u8 *s, va_list *va)
{
  vnet_main_t *vnm = va_arg(*va, vnet_main_t *);
  ila_entry_t *e = va_arg(*va, ila_entry_t *);

  if(!e) {
    return format(s, "%=20s%=20s%=20s%=16s%=18s", "Identifier", "Locator", "SIR prefix", "Adjacency Index", "Checksum Mode");
  } else if(vnm) {
    if(e->ila_adj_index == ~0) {
      return format(s, "%U %U %U %15s    %U",
        format_half_ip6_address, e->identifier,
        format_half_ip6_address, e->locator,
        format_half_ip6_address, e->sir_prefix,
        "n/a",
        format_csum_mode, e->csum_mode);
    } else {
      return format(s, "%U %U %U %15d    %U",
        format_half_ip6_address, e->identifier,
        format_half_ip6_address, e->locator,
        format_half_ip6_address, e->sir_prefix,
        e->ila_adj_index,
        format_csum_mode, e->csum_mode);
    }
  }

  return NULL;
}

u8 *
format_ila_ila2sir_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED(vlib_main_t *vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED(vlib_node_t *node) = va_arg (*args, vlib_node_t *);
  ila_ila2sir_trace_t *t = va_arg (*args, ila_ila2sir_trace_t *);
  return format(s, "ILA -> SIR entry index: %d initial_dst: %U", t->ila_index,
                format_ip6_address, &t->initial_dst);
}

static uword
unformat_ila_csum_mode (unformat_input_t * input, va_list * args)
{
  ila_csum_mode_t * result = va_arg (*args, ila_csum_mode_t *);
  if (unformat(input, "none") || unformat(input, "no-action"))
    {
      *result = ILA_CSUM_MODE_NO_ACTION;
      return 1;
    }
  if (unformat(input, "neutral-map"))
    {
      *result = ILA_CSUM_MODE_NEUTRAL_MAP;
      return 1;
    }
  if (unformat(input, "adjust-transport"))
    {
      *result = ILA_CSUM_MODE_ADJUST_TRANSPORT;
      return 1;
    }
  return 0;
}

static uword
unformat_half_ip6_address (unformat_input_t * input, va_list * args)
{
  u64 * result = va_arg (*args, u64 *);
  u32 a[4];

  if (! unformat (input, "%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3]))
    return 0;

  if (a[0] > 0xFFFF || a[1] > 0xFFFF || a[2] > 0xFFFF|| a[3] > 0xFFFF)
    return 0;

  *result = clib_host_to_net_u64((((u64)a[0]) << 48) |
                                 (((u64)a[1]) << 32) |
                                 (((u64)a[2]) << 16) |
                                 (((u64)a[3])));

  return 1;
}

static_always_inline void ila_adjust_csum_ila2sir(ila_entry_t *e, ip6_address_t *addr)
{
      ip_csum_t csum = addr->as_u16[7];
      csum = ip_csum_sub_even(csum, e->csum_modifier);
      addr->as_u16[7] = ip_csum_fold(csum);
      addr->as_u8[8] &= 0xef;
}

static_always_inline void ila_adjust_csum_sir2ila(ila_entry_t *e, ip6_address_t *addr)
{
      ip_csum_t csum = addr->as_u16[7];
      csum = ip_csum_add_even(csum, e->csum_modifier);
      addr->as_u16[7] = ip_csum_fold(csum);
      addr->as_u8[8] |= 0x10;
}

static vlib_node_registration_t ila_ila2sir_node;

static uword
ila_ila2sir (vlib_main_t *vm,
        vlib_node_runtime_t *node,
        vlib_frame_t *frame)
{
  ip4_main_t * im = &ip4_main;
  ip_lookup_main_t * lm = &im->lookup_main;
  u32 n_left_from, *from, next_index, *to_next, n_left_to_next;
  vlib_node_runtime_t *error_node = vlib_node_get_runtime(vm, ila_ila2sir_node.index);
  ila_main_t *ilm = &ila_main;

  from = vlib_frame_vector_args(frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0) {
    vlib_get_next_frame(vm, node, next_index, to_next, n_left_to_next);

    /* Single loop */
    while (n_left_from > 0 && n_left_to_next > 0) {
      u32 pi0;
      vlib_buffer_t * p0;
      ip_adjacency_t * adj0;
      ila_entry_t * ie0;
      u8 error0 = ILA_ERROR_NONE;
      ip6_header_t *ip60;
      u32 next0 = ILA_ILA2SIR_NEXT_IP6_REWRITE;

      pi0 = to_next[0] = from[0];
      from += 1;
      n_left_from -= 1;
      to_next +=1;
      n_left_to_next -= 1;

      p0 = vlib_get_buffer(vm, pi0);
      ip60 = vlib_buffer_get_current(p0);
      adj0 = ip_get_adjacency (lm, vnet_buffer(p0)->ip.adj_index[VLIB_TX]);
      ie0 = pool_elt_at_index (ilm->entries, adj0->ila.entry_index);

      if (PREDICT_FALSE(p0->flags & VLIB_BUFFER_IS_TRACED)) {
          ila_ila2sir_trace_t *tr = vlib_add_trace(vm, node, p0, sizeof(*tr));
          tr->ila_index = ie0?(ie0 - ilm->entries):~0;
          tr->initial_dst = ip60->dst_address;
      }

      ip60->dst_address.as_u64[0] = ie0->sir_prefix;
      vnet_buffer(p0)->ip.adj_index[VLIB_TX] = ie0->ila_adj_index;

      if (ie0->csum_mode == ILA_CSUM_MODE_NEUTRAL_MAP)
        ila_adjust_csum_ila2sir(ie0, &ip60->dst_address);

      p0->error = error_node->errors[error0];
      vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next, n_left_to_next, pi0, next0);
    }
    vlib_put_next_frame(vm, node, next_index, n_left_to_next);
  }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (ila_ila2sir_node, static) = {
  .function = ila_ila2sir,
  .name = "ila-ila2sir",
  .vector_size = sizeof (u32),

  .format_trace = format_ila_ila2sir_trace,

  .n_errors = ILA_N_ERROR,
  .error_strings = ila_error_strings,

  .n_next_nodes = ILA_ILA2SIR_N_NEXT,
  .next_nodes = {
      [ILA_ILA2SIR_NEXT_IP6_REWRITE] = "ip6-rewrite",
      [ILA_ILA2SIR_NEXT_DROP] = "error-drop"
    },
};

typedef enum {
  ILA_SIR2ILA_NEXT_DROP,
  ILA_SIR2ILA_N_NEXT,
} ila_sir2ila_next_t;

typedef struct {
  u32 ila_index;
  ip6_address_t initial_dst;
} ila_sir2ila_trace_t;

u8 *
format_ila_sir2ila_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED(vlib_main_t *vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED(vlib_node_t *node) = va_arg (*args, vlib_node_t *);
  ila_sir2ila_trace_t *t = va_arg (*args, ila_sir2ila_trace_t *);

  return format(s, "SIR -> ILA entry index: %d initial_dst: %U", t->ila_index,
                format_ip6_address, &t->initial_dst);
}

static vlib_node_registration_t ila_sir2ila_node;

static uword
ila_sir2ila (vlib_main_t *vm,
        vlib_node_runtime_t *node,
        vlib_frame_t *frame)
{
  ip6_main_t * im = &ip6_main;
  ip_lookup_main_t * lm = &im->lookup_main;
  ip_config_main_t * cm = &lm->rx_config_mains[VNET_UNICAST];
  u32 n_left_from, *from, next_index, *to_next, n_left_to_next;
  vlib_node_runtime_t *error_node = vlib_node_get_runtime(vm, ila_sir2ila_node.index);
  ila_main_t *ilm = &ila_main;

  from = vlib_frame_vector_args(frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0) {
    vlib_get_next_frame(vm, node, next_index, to_next, n_left_to_next);

    /* Single loop */
    while (n_left_from > 0 && n_left_to_next > 0) {
      u32 pi0;
      vlib_buffer_t *p0;
      u8 error0 = ILA_ERROR_NONE;
      ip6_header_t *ip60;
      u32 next0 = ILA_SIR2ILA_NEXT_DROP;
      BVT(clib_bihash_kv) kv, value;
      ila_entry_t *e = NULL;

      pi0 = to_next[0] = from[0];
      from += 1;
      n_left_from -= 1;
      to_next +=1;
      n_left_to_next -= 1;

      p0 = vlib_get_buffer(vm, pi0);
      ip60 = vlib_buffer_get_current(p0);
      kv.key = ip60->dst_address.as_u64[1];

      if ((BV(clib_bihash_search)(&ilm->id_to_entry_table, &kv, &value)) == 0)
        {
          e = &ilm->entries[value.value];
        }

      if (PREDICT_FALSE(p0->flags & VLIB_BUFFER_IS_TRACED)) {
          ila_sir2ila_trace_t *tr = vlib_add_trace(vm, node, p0, sizeof(*tr));
          tr->ila_index = e?(e - ilm->entries):~0;
          tr->initial_dst = ip60->dst_address;
      }

      ip60->dst_address.as_u64[0] = e?e->locator:ip60->dst_address.as_u64[0];

      if (e->csum_mode == ILA_CSUM_MODE_NEUTRAL_MAP)
        ila_adjust_csum_sir2ila(e, &ip60->dst_address);

      vnet_get_config_data (&cm->config_main,
                            &p0->current_config_index,
                            &next0, 0);

      p0->error = error_node->errors[error0];
      vlib_validate_buffer_enqueue_x1(vm, node, next_index, to_next, n_left_to_next, pi0, next0);
    }
    vlib_put_next_frame(vm, node, next_index, n_left_to_next);
  }

  return frame->n_vectors;
}

VLIB_REGISTER_NODE (ila_sir2ila_node, static) = {
  .function = ila_sir2ila,
  .name = "ila-sir2ila",
  .vector_size = sizeof (u32),

  .format_trace = format_ila_sir2ila_trace,

  .n_errors = ILA_N_ERROR,
  .error_strings = ila_error_strings,

  .n_next_nodes = ILA_SIR2ILA_N_NEXT,
  .next_nodes = {
      [ILA_SIR2ILA_NEXT_DROP] = "error-drop"
    },
};

VNET_IP6_UNICAST_FEATURE_INIT (ila_sir2ila, static) = {
  .node_name = "ila-sir2ila",
  .runs_before = {"ip6-lookup", 0},
  .feature_index = &ila_main.ila_sir2ila_feature_index,
};

int ila_add_del_entry(ila_add_del_entry_args_t *args)
{
  ila_main_t *ilm = &ila_main;
  ip6_main_t *im6 = &ip6_main;
  BVT(clib_bihash_kv) kv, value;

  if (!args->is_del)
    {
      ila_entry_t *e;
      pool_get(ilm->entries, e);
      e->identifier = args->identifier;
      e->locator = args->locator;
      e->ila_adj_index = args->local_adj_index;
      e->sir_prefix = args->sir_prefix;
      e->csum_mode = args->csum_mode;

      kv.key = args->identifier;
      kv.value = e - ilm->entries;
      BV(clib_bihash_add_del) (&ilm->id_to_entry_table, &kv, 1 /* is_add */);

      ip_csum_t csum = 0xffff;
      csum = ip_csum_add_even(csum, e->locator >> 32);
      csum = ip_csum_add_even(csum, (u32) e->locator);
      csum = ip_csum_add_even(csum, clib_host_to_net_u16(0x1000));
      csum = ip_csum_sub_even(csum, e->sir_prefix >> 32);
      csum = ip_csum_sub_even(csum, (u32)e->sir_prefix);
      e->csum_modifier = ~ip_csum_fold(csum);

      if (e->ila_adj_index != ~0)
        {
          //This is a local entry - let's create a local adjacency
          ip_adjacency_t adj;
          ip6_add_del_route_args_t route_args;

          //Adjacency
          memset(&adj, 0, sizeof(adj));
          adj.explicit_fib_index = ~0;
          adj.lookup_next_index = IP6_LOOKUP_NEXT_ILA;
	  adj.ila.entry_index = e - ilm->entries;

          //Route
          memset(&route_args, 0, sizeof(route_args));
          route_args.table_index_or_table_id = 0;
          route_args.flags = IP6_ROUTE_FLAG_ADD;
          route_args.dst_address.as_u64[0] = args->locator;
          route_args.dst_address.as_u64[1] = args->identifier;
          route_args.dst_address_length = 128;
          route_args.adj_index = ~0;
          route_args.add_adj = &adj;
          route_args.n_add_adj = 1;

          //Change destination if csum neutral is enabled
          if (e->csum_mode == ILA_CSUM_MODE_NEUTRAL_MAP)
            ila_adjust_csum_sir2ila(e, &route_args.dst_address);

          ip6_add_del_route(im6, &route_args);
        }
    }
  else
    {
      ila_entry_t *e;
      kv.key = args->identifier;

      if ((BV(clib_bihash_search)(&ilm->id_to_entry_table, &kv, &value) < 0))
        {
          return -1;
        }

      e = &ilm->entries[value.value];

      if (e->ila_adj_index != ~0)
        {
          //Delete that route - Associated adjacency will be deleted too
          ip6_add_del_route_args_t route_args;
          memset(&route_args, 0, sizeof(route_args));
          route_args.table_index_or_table_id = 0;
          route_args.flags = IP6_ROUTE_FLAG_DEL;
          route_args.dst_address.as_u64[0] = args->locator;
          route_args.dst_address.as_u64[1] = args->identifier;
          route_args.dst_address_length = 128;
          route_args.adj_index = ~0;
          route_args.add_adj = NULL;
          route_args.n_add_adj = 0;
          ip6_add_del_route(im6, &route_args);
        }

      BV(clib_bihash_add_del) (&ilm->id_to_entry_table, &kv, 0 /* is_add */);
      pool_put(ilm->entries, e);
    }
  return 0;
}

int ila_interface(u32 sw_if_index, u8 disable)
{
  vlib_main_t * vm = vlib_get_main();
  ila_main_t *ilm = &ila_main;
  ip6_main_t * im = &ip6_main;
  ip_lookup_main_t * lm = &im->lookup_main;
  ip_config_main_t * cm = &lm->rx_config_mains[VNET_UNICAST];
  vnet_config_main_t * vcm = &cm->config_main;
  u32 ci, feature_index;

  vec_validate_init_empty (cm->config_index_by_sw_if_index, sw_if_index, ~0);
  ci = cm->config_index_by_sw_if_index[sw_if_index];
  feature_index = ilm->ila_sir2ila_feature_index;

  ci = ((disable)?vnet_config_del_feature:vnet_config_add_feature)
          (vm, vcm,
              ci,
              feature_index,
              /* config data */ 0,
              /* # bytes of config data */ 0);

  cm->config_index_by_sw_if_index[sw_if_index] = ci;
  return 0;
}

clib_error_t *ila_init (vlib_main_t *vm) {
  ila_main_t *ilm = &ila_main;
  ilm->entries = NULL;

  ilm->lookup_table_nbuckets = ILA_TABLE_DEFAULT_HASH_NUM_BUCKETS;
  ilm->lookup_table_nbuckets = 1<< max_log2 (ilm->lookup_table_nbuckets);
  ilm->lookup_table_size = ILA_TABLE_DEFAULT_HASH_MEMORY_SIZE;

  BV(clib_bihash_init) (&ilm->id_to_entry_table, "ila id to entry index table",
                          ilm->lookup_table_nbuckets,
                          ilm->lookup_table_size);

  return NULL;
}

VLIB_INIT_FUNCTION(ila_init);

static clib_error_t *
ila_entry_command_fn (vlib_main_t *vm,
                          unformat_input_t *input,
                          vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, * line_input = &_line_input;
  ila_add_del_entry_args_t args = {0};
  int ret;

  args.csum_mode = ILA_CSUM_MODE_NO_ACTION;
  args.local_adj_index = ~0;

  if (! unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input(line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "%U %U %U",
                    unformat_half_ip6_address, &args.identifier,
                    unformat_half_ip6_address, &args.locator,
                    unformat_half_ip6_address, &args.sir_prefix))
        ;
      else if (unformat (line_input, "del"))
        args.is_del = 1;
      else if (unformat (line_input, "adj-index %u", &args.local_adj_index))
        ;
      else if (unformat (line_input, "csum-mode %U", unformat_ila_csum_mode, &args.csum_mode))
        ;
      else
        return clib_error_return (0, "parse error: '%U'",
                                  format_unformat_error, line_input);
  }

  unformat_free (line_input);

  if ((ret = ila_add_del_entry(&args)))
    return clib_error_return (0, "ila_add_del_entry returned error %d", ret);

  return NULL;
}

VLIB_CLI_COMMAND(ila_entry_command, static) = {
  .path = "ila entry",
  .short_help = "ila entry <identifier> <locator> <sir-prefix> [adj-index <adj-index>] "
      "[csum-mode (no-action|neutral-map|transport-adjust)] [del] ",
  .function = ila_entry_command_fn,
};

static clib_error_t *
ila_interface_command_fn (vlib_main_t *vm,
                          unformat_input_t *input,
                          vlib_cli_command_t *cmd)
{
  vnet_main_t * vnm = vnet_get_main();
  u32 sw_if_index = ~0;
  u8 disable = 0;

  if (!unformat (input, "%U", unformat_vnet_sw_interface,
                 vnm, &sw_if_index)) {
      return clib_error_return (0, "Invalid interface name");
  }

  if (unformat(input, "disable")) {
      disable = 1;
  }

  int ret;
  if ((ret = ila_interface(sw_if_index, disable)))
    return clib_error_return (0, "ila_interface returned error %d", ret);

  return NULL;
}

VLIB_CLI_COMMAND(ila_interface_command, static) = {
  .path = "ila interface",
  .short_help = "ila interface <interface-name> [disable]",
  .function = ila_interface_command_fn,
};

static clib_error_t *
ila_show_entries_command_fn (vlib_main_t *vm,
                             unformat_input_t *input,
                             vlib_cli_command_t *cmd)
{
  vnet_main_t * vnm = vnet_get_main();
  ila_main_t *ilm = &ila_main;
  ila_entry_t *e;

  vlib_cli_output(vm, "  %U\n", format_ila_entry, vnm, NULL);
  pool_foreach(e, ilm->entries, ({
    vlib_cli_output(vm, "  %U\n", format_ila_entry, vnm, e);
  }));

  return NULL;
}

VLIB_CLI_COMMAND(ila_show_entries_command, static) = {
  .path = "ila show",
  .short_help = "show ila entries",
  .function = ila_show_entries_command_fn,
};


static clib_error_t *
test_ila_addresses_fn (vlib_main_t *vm,
                             unformat_input_t *input,
                             vlib_cli_command_t *cmd)
{
  vnet_main_t * vnm = vnet_get_main();
  ila_main_t *ilm = &ila_main;
  u32 entry_index = 0;
  ila_entry_t *e = NULL;

  if (!unformat(input, "%d", &entry_index) || pool_is_free_index(ilm->entries, entry_index))
      return clib_error_return (0, "Invalid entry index");

  e = &ilm->entries[entry_index];
  ip6_address_t sir_address, ila_address;
  sir_address.as_u64[0] = e->sir_prefix;
  sir_address.as_u64[1] = e->identifier;
  ila_address.as_u64[0] = e->locator;
  ila_address.as_u64[1] = e->identifier;

  if (e->csum_mode == ILA_CSUM_MODE_NEUTRAL_MAP)
    ila_adjust_csum_sir2ila(e, &ila_address);

  vlib_cli_output(vm, "        %U\n", format_ila_entry, vnm, NULL);
  vlib_cli_output(vm, "entry:  %U\n", format_ila_entry, vnm, e);
  vlib_cli_output(vm, "sir address: %U\n", format_ip6_address, &sir_address);
  vlib_cli_output(vm, "ila address: %U\n", format_ip6_address, &ila_address);

  return NULL;
}

VLIB_CLI_COMMAND(test_ila_addresses, static) = {
  .path = "test ila addresses",
  .short_help = "test ila addresses <ila_entry_index>",
  .function = test_ila_addresses_fn,
};



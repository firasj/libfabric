#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER EFA_TP_PROV

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "efa_tp_def.h"

#if !defined(_EFA_TP_DEF_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _EFA_TP_DEF_H

#include <lttng/tracepoint.h>

#define EFA_TP_PROV efa

#define X_PKT_ARGS \
	size_t, wr_id, \
	size_t, rxr_op_entry, \
	size_t, context

#define X_PKT_FIELDS \
	lttng_ust_field_integer_hex(size_t, wr_id, wr_id) \
	lttng_ust_field_integer_hex(size_t, rxr_op_entry, rxr_op_entry) \
	lttng_ust_field_integer_hex(size_t, context, context)

LTTNG_UST_TRACEPOINT_EVENT(
	EFA_TP_PROV,
	post_send,
	LTTNG_UST_TP_ARGS(
		X_PKT_ARGS
	),
	LTTNG_UST_TP_FIELDS(
		X_PKT_FIELDS  
	)
)
LTTNG_UST_TRACEPOINT_LOGLEVEL(EFA_TP_PROV, post_send, LTTNG_UST_TRACEPOINT_LOGLEVEL_INFO)

LTTNG_UST_TRACEPOINT_EVENT(
	EFA_TP_PROV,
	post_recv,
	LTTNG_UST_TP_ARGS(
		X_PKT_ARGS
	),
	LTTNG_UST_TP_FIELDS(
		X_PKT_FIELDS  
	)
)
LTTNG_UST_TRACEPOINT_LOGLEVEL(EFA_TP_PROV, post_recv, LTTNG_UST_TRACEPOINT_LOGLEVEL_INFO)

#endif // _EFA_TP_H

#include <lttng/tracepoint-event.h>
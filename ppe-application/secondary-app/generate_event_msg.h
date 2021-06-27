#ifdef __cplusplus
extern "C" {
#endif

#include "deepstream_app.h"
#include "gstnvdsmeta.h"
#include "nvdsmeta_schema.h"

NvDsEventMsgMeta* generate_event_msg_from_frame(AppCtx * appCtx, NvDsFrameMeta * frame_meta);
gpointer meta_copy_func(gpointer data, gpointer user_date);
void meta_free_func(gpointer data, gpointer user_data);

#ifdef __cplusplus
}
#endif
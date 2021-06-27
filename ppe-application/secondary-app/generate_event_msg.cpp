#include <sstream>
#include <glib.h>
#include "generate_event_msg.h"
#include "nvds_analytics_meta.h"
#include <time.h>

#define MAX_TIME_STAMP_LEN (64) 

static 
char* generate_ts_rfc3339()
{
  char *buf = (char*) g_malloc0(MAX_TIME_STAMP_LEN + 1); //64 is the maximum size of the timestamp
  time_t tloc;
  struct tm tm_log;
  struct timespec ts;
  char strmsec[6];

  clock_gettime (CLOCK_REALTIME, &ts);
  memcpy (&tloc, (void *) (&ts.tv_sec), sizeof (time_t));
  gmtime_r (&tloc, &tm_log);
  strftime (buf, MAX_TIME_STAMP_LEN, "%Y-%m-%dT%H:%M:%S", &tm_log);
  int ms = ts.tv_nsec / 1000000;
  g_snprintf (strmsec, sizeof (strmsec), ".%.3dZ", ms);
  strncat (buf, strmsec, MAX_TIME_STAMP_LEN);
  return buf;
}

extern "C"
NvDsEventMsgMeta* generate_event_msg_from_frame(AppCtx * appCtx, NvDsFrameMeta * frame_meta)
{
  GList *obj_min_meta_list = NULL;
  NvDsObjectMinimalMeta *obj_min_meta = NULL;

  GList *l;
  
  GList *obj_min_meta2_list = NULL;
  
  

  for (l = frame_meta->obj_meta_list; l != NULL; l = l->next) {
    
    NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) (l->data);

    if (!appCtx->config.streammux_config.pipeline_width || !appCtx->config.streammux_config.pipeline_height) {
      g_print ("invalid pipeline params\n");
      return NULL;
    }

    float scaleW = (float) frame_meta->source_frame_width / appCtx->config.streammux_config.pipeline_width;
    float scaleH = (float) frame_meta->source_frame_height / appCtx->config.streammux_config.pipeline_height;

  
    
     


    obj_min_meta = (NvDsObjectMinimalMeta *) g_malloc0 (sizeof(NvDsObjectMinimalMeta));

    obj_min_meta->left = obj_meta->rect_params.left * scaleW;
    obj_min_meta->top = obj_meta->rect_params.top * scaleH;
    obj_min_meta->width = obj_meta->rect_params.width * scaleW;
    obj_min_meta->height = obj_meta->rect_params.height * scaleH;
    obj_min_meta->class_id = obj_meta->class_id;
    obj_min_meta->object_id = obj_meta->object_id;
    obj_min_meta->confidence = obj_meta->confidence;
    obj_min_meta->tracker_confidence = obj_meta->tracker_confidence;
    obj_min_meta->obj_label = g_strdup(obj_meta->obj_label);

    obj_min_meta->unique_component_id = obj_meta->unique_component_id;
    
    
    
    obj_min_meta->parent_object_id = NULL;
    if (obj_meta->unique_component_id == 2) {
      obj_min_meta->parent_object_id = obj_meta->parent->object_id;
     }
    
    

    obj_min_meta->obj_dir = NULL;
    GList *obj_line_list = NULL;
    int lcStatusSize = 0;
    int roiStatusSize = 0;
    for (NvDsMetaList *l_user_meta = obj_meta->obj_user_meta_list; l_user_meta!=NULL; l_user_meta=l_user_meta->next)
    {
      NvDsUserMeta *user_meta = (NvDsUserMeta*) l_user_meta->data;
      if(user_meta->base_meta.meta_type == NVDS_USER_OBJ_META_NVDSANALYTICS)
      {
        NvDsAnalyticsObjInfo *user_meta_data = (NvDsAnalyticsObjInfo*) user_meta->user_meta_data;
        


        lcStatusSize = user_meta_data->lcStatus.size();
        if(lcStatusSize){
          for(int i=0; i<lcStatusSize; i++){
            obj_min_meta->obj_cross = g_strdup(user_meta_data->lcStatus[i].c_str()); 
            }
        }
      }
    }
    obj_min_meta_list = g_list_prepend (obj_min_meta_list, (gpointer*) obj_min_meta); 
  }

 
  
  NvDsEventMsgMeta *msg_meta = (NvDsEventMsgMeta*) g_malloc0(sizeof(NvDsEventMsgMeta));
  
  msg_meta->streamId = frame_meta->source_id;
  msg_meta->frameId = frame_meta->frame_num;
  msg_meta->ts = (char*) generate_ts_rfc3339();
  msg_meta->sourceUri = g_strdup(appCtx->config.multi_source_config[frame_meta->source_id].uri);
  
  if(obj_min_meta_list)
  {
    msg_meta->obj_min_meta_list = g_list_reverse (obj_min_meta_list);
    
  }

  
  if(obj_min_meta_list)
  {
    for (GList *l = msg_meta->obj_min_meta_list; l!=NULL; l=l->next)
    {     
      NvDsObjectMinimalMeta *obj_min_meta = (NvDsObjectMinimalMeta*) l->data;
      if(obj_min_meta->obj_cross) {
        for (GList *l2 = msg_meta->obj_min_meta_list; l2!=NULL; l2=l2->next)
          {     
              NvDsObjectMinimalMeta *obj_min_meta2 = (NvDsObjectMinimalMeta*) l2->data;
              if (obj_min_meta2->unique_component_id == 2){
                  if (obj_min_meta2->parent_object_id == obj_min_meta->object_id ){
                    if(obj_min_meta2->class_id == 0 ){
                      if (!obj_min_meta->helmet){
                      obj_min_meta->helmet = obj_min_meta2->obj_label;
                      }
                    }
                  }
              }
              obj_min_meta2_list = g_list_prepend (obj_min_meta2_list, (gpointer*) obj_min_meta2);
              }
    }
   }
  
  }

  if(obj_min_meta2_list)
  {
    for (GList *l = obj_min_meta2_list; l!=NULL; l=l->next)
    {     
      NvDsObjectMinimalMeta *obj_min_meta = (NvDsObjectMinimalMeta*) l->data;
      if (obj_min_meta->unique_component_id == 1){
         if(obj_min_meta->obj_cross){
           if(obj_min_meta->helmet){
              g_print("   %s ID:%d crossed %s with %s\n",obj_min_meta->obj_label,obj_min_meta->object_id,obj_min_meta->obj_cross,obj_min_meta->helmet);
           }
           else {
              g_print("   %s ID:%d crossed %s without helmet\n",obj_min_meta->obj_label,obj_min_meta->object_id,obj_min_meta->obj_cross);
            }
          }

    }
  }
  }

  GList *line_name_list = NULL, *line_count_list = NULL; 
  for(NvDsMetaList *l_user_meta = frame_meta->frame_user_meta_list; l_user_meta!=NULL; l_user_meta=l_user_meta->next)
  {
    NvDsUserMeta *user_meta = (NvDsUserMeta *) l_user_meta->data;
    if (user_meta->base_meta.meta_type == NVDS_USER_FRAME_META_NVDSANALYTICS)
    {
      NvDsAnalyticsFrameMeta *meta = (NvDsAnalyticsFrameMeta *) user_meta->user_meta_data;
      for(std::pair<std::string, uint64_t> status : meta->objLCCumCnt)
      {
        line_name_list = g_list_prepend(line_name_list, g_strdup(status.first.c_str()));
        line_count_list = g_list_prepend(line_count_list, GUINT_TO_POINTER (status.second));
      }

      for(std::pair<std::string, uint64_t> status : meta->objInROIcnt)
      {
        line_name_list = g_list_prepend(line_name_list, g_strdup(status.first.c_str()));
        
      }
    }
    
    msg_meta->line_name_list = g_list_reverse(line_name_list);
    msg_meta->line_count_list = g_list_reverse(line_count_list);
    
    return msg_meta;
  } 

}

extern "C"
gpointer meta_copy_func(gpointer data, gpointer user_date)
{
  NvDsUserMeta *user_meta = (NvDsUserMeta *) data;
  NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *) user_meta->user_meta_data;
  NvDsEventMsgMeta *dstMeta = NULL;

  dstMeta = (NvDsEventMsgMeta*) g_memdup(srcMeta, sizeof (NvDsEventMsgMeta));
  dstMeta->obj_min_meta_list = NULL;
  dstMeta->line_name_list = NULL;
  dstMeta->line_count_list = NULL;

  if (srcMeta->ts)
    dstMeta->ts = g_strdup (srcMeta->ts);
  
  if (srcMeta->sourceUri)
    dstMeta->sourceUri = g_strdup(srcMeta->sourceUri);
  
  if (srcMeta->obj_min_meta_list)
  {
    GList *l;
    for (l = srcMeta->obj_min_meta_list; l!=NULL; l=l->next)
    {

      NvDsObjectMinimalMeta *src_obj_min_meta = (NvDsObjectMinimalMeta *) l->data;
      NvDsObjectMinimalMeta *dst_obj_min_meta = (NvDsObjectMinimalMeta*) g_memdup(src_obj_min_meta, sizeof(NvDsObjectMinimalMeta));
      dst_obj_min_meta->obj_line_list = NULL;

      if(src_obj_min_meta->obj_label)
        dst_obj_min_meta->obj_label = g_strdup(src_obj_min_meta->obj_label);
      
      if(src_obj_min_meta->obj_dir)
        dst_obj_min_meta->obj_dir = g_strdup(src_obj_min_meta->obj_dir);
      
      if(src_obj_min_meta->obj_line_list)
        dst_obj_min_meta->obj_line_list = g_list_copy_deep(src_obj_min_meta->obj_line_list, (GCopyFunc) g_strdup, NULL);
  
      dstMeta->obj_min_meta_list = g_list_prepend (dstMeta->obj_min_meta_list, (gpointer*) dst_obj_min_meta);
    }
    dstMeta->obj_min_meta_list = g_list_reverse(dstMeta->obj_min_meta_list);
  }

  if (srcMeta->line_name_list)
    dstMeta->line_name_list = g_list_copy_deep(srcMeta->line_name_list, (GCopyFunc) g_strdup, NULL);
  
  if (srcMeta->line_count_list)
    dstMeta->line_count_list = g_list_copy(srcMeta->line_count_list);

  return dstMeta; 
}

extern "C"
void meta_free_func(gpointer data, gpointer user_data)
{
  NvDsUserMeta *user_meta = (NvDsUserMeta *) data;
  NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *) user_meta->user_meta_data;
  user_meta->user_meta_data = NULL;

  if (srcMeta->ts)
    g_free (srcMeta->ts);
  
  if (srcMeta->sourceUri)
    g_free (srcMeta->sourceUri);
  
  if (srcMeta->obj_min_meta_list)
  {
    GList *l;
    for (l = srcMeta->obj_min_meta_list; l!=NULL; l=l->next)
    {
      NvDsObjectMinimalMeta *src_obj_min_meta = (NvDsObjectMinimalMeta *) l->data;
      if(src_obj_min_meta->obj_label)
        g_free (src_obj_min_meta->obj_label);

      if(src_obj_min_meta->obj_dir)
        g_free (src_obj_min_meta->obj_dir);
      
      if(src_obj_min_meta->obj_line_list)
        g_list_free_full(src_obj_min_meta->obj_line_list, g_free);
      
      g_free (src_obj_min_meta);
    }

    g_list_free (srcMeta->obj_min_meta_list);
  }

  if (srcMeta->line_name_list)
    g_list_free_full(srcMeta->line_name_list, g_free);
  
  if (srcMeta->line_count_list)
    g_list_free(srcMeta->line_count_list);

  g_free (srcMeta);  
}

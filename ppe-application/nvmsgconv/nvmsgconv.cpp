/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA Corporation is strictly prohibited.
 *
 */

#include "nvmsgconv.h"
#include <json-glib/json-glib.h>
#include <uuid.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <vector>
#include <unordered_map>

using namespace std;


#define CONFIG_GROUP_SENSOR "sensor"
#define CONFIG_GROUP_PLACE "place"
#define CONFIG_GROUP_ANALYTICS "analytics"

#define CONFIG_KEY_COORDINATE "coordinate"
#define CONFIG_KEY_DESCRIPTION "description"
#define CONFIG_KEY_ENABLE  "enable"
#define CONFIG_KEY_ID "id"
#define CONFIG_KEY_LANE "lane"
#define CONFIG_KEY_LEVEL "level"
#define CONFIG_KEY_LOCATION "location"
#define CONFIG_KEY_NAME "name"
#define CONFIG_KEY_SOURCE "source"
#define CONFIG_KEY_TYPE "type"
#define CONFIG_KEY_VERSION "version"


#define CONFIG_KEY_PLACE_SUB_FIELD1 "place-sub-field1"
#define CONFIG_KEY_PLACE_SUB_FIELD2 "place-sub-field2"
#define CONFIG_KEY_PLACE_SUB_FIELD3 "place-sub-field3"

#define DEFAULT_CSV_FIELDS 10


#define CHECK_ERROR(error) \
    if (error) { \
      cout << "Error: " << error->message << endl; \
      goto done; \
    }


/**
 * Based on place type field of this object will have different meaning.
 * e.g. field1 will be 'id' and 'name' for spot and entrance respectively.
 */
struct NvDsPlaceSubObject {
  string field1;
  string field2;
  string field3;
};

struct NvDsSensorObject {
  string id;
  string type;
  string desc;
  gdouble location[3];
  gdouble coordinate[3];
};

struct NvDsPlaceObject {
  string id;
  string name;
  string type;
  gdouble location[3];
  gdouble coordinate[3];
  NvDsPlaceSubObject subObj;
};

struct NvDsAnalyticsObject {
  string id;
  string desc;
  string source;
  string version;
};

struct NvDsPayloadPriv {
  unordered_map<int, NvDsSensorObject> sensorObj;
  unordered_map<int, NvDsPlaceObject> placeObj;
  unordered_map<int, NvDsAnalyticsObject> analyticsObj;
};

static void
get_csv_tokens (const string &text, vector<string> &tokens)
{
  /* This is based on assumption that fields and their locations
   * are fixed in CSV file. This should be updated accordingly if
   * that is not the case.
   */
  gint count = 0;

  gchar **csv_tokens = g_strsplit (text.c_str(), ",", -1);
  gchar **temp = csv_tokens;
  gchar *token;

  while (*temp && count < DEFAULT_CSV_FIELDS) {
    token = *temp++;
    tokens.push_back (string(g_strstrip(token)));
    count++;
  }
  g_strfreev (csv_tokens);
}


static JsonObject*
generate_frame_object (NvDsMsg2pCtx *ctx, NvDsEventMsgMeta *meta) {
  JsonObject *frameObj;
  JsonObject *jobject;
  JsonObject *jobject2;

  frameObj = json_object_new();
  json_object_set_int_member (frameObj, "stream_id", meta->streamId);
  json_object_set_int_member (frameObj, "frame_id", meta->frameId);
  json_object_set_string_member (frameObj, "source", meta->sourceUri);

  if (meta->obj_min_meta_list)
  {
    guint i=0;
    gchar str_obj_name[128];

    for (GList *l = meta->obj_min_meta_list; l!=NULL; l=l->next)
    {     
      jobject = json_object_new();
      NvDsObjectMinimalMeta *obj_min_meta = (NvDsObjectMinimalMeta*) l->data;

      
      gchar tracking_id[64];
      if (snprintf (tracking_id, sizeof(tracking_id), "%lu", obj_min_meta->object_id) >= (int) sizeof(tracking_id))
        g_warning("Not enough space to copy trackingId");

      json_object_set_int_member (jobject, "top", obj_min_meta->top);
      json_object_set_int_member (jobject, "left", obj_min_meta->left);
      json_object_set_int_member (jobject, "width", obj_min_meta->width);
      json_object_set_int_member (jobject, "height", obj_min_meta->height);

      json_object_set_int_member (jobject, "class_id", obj_min_meta->class_id);
      
      
      json_object_set_string_member (jobject, "object_id", tracking_id);

      json_object_set_double_member (jobject, "confidence", obj_min_meta->confidence);
      json_object_set_double_member (jobject, "tracker_confidence", obj_min_meta->tracker_confidence);

      json_object_set_string_member (jobject, "obj_label", obj_min_meta->obj_label);
    }
  }

  return frameObj;
}

static gchar*
generate_schema_message (NvDsMsg2pCtx *ctx, NvDsEventMsgMeta *meta)
{
  JsonNode *rootNode;
  JsonObject *rootObj;
  JsonObject *frameObj;
  gchar *message;

  uuid_t msgId;
  gchar msgIdStr[37];

  uuid_generate_random (msgId);
  uuid_unparse_lower(msgId, msgIdStr);

  // frame object
  frameObj = generate_frame_object (ctx, meta);

  // root object
  rootObj = json_object_new ();
  json_object_set_string_member (rootObj, "messageid", msgIdStr);
  json_object_set_string_member (rootObj, "@timestamp", meta->ts);
  json_object_set_object_member (rootObj, "frame_info", frameObj);

  rootNode = json_node_new (JSON_NODE_OBJECT);
  json_node_set_object (rootNode, rootObj);

  message = json_to_string (rootNode, TRUE);
  json_node_free (rootNode);
  json_object_unref (rootObj);

  return message;
}

static const gchar*
object_enum_to_str (NvDsObjectType type, gchar* objectId)
{
  switch (type) {
    case NVDS_OBJECT_TYPE_VEHICLE:
      return "Vehicle";
    case NVDS_OBJECT_TYPE_FACE:
      return "Face";
    case NVDS_OBJECT_TYPE_PERSON:
      return "Person";
    case NVDS_OBJECT_TYPE_BAG:
      return "Bag";
    case NVDS_OBJECT_TYPE_BICYCLE:
      return "Bicycle";
    case NVDS_OBJECT_TYPE_ROADSIGN:
      return "RoadSign";
    case NVDS_OBJECT_TYPE_CUSTOM:
      return "Custom";
    case NVDS_OBJECT_TYPE_UNKNOWN:
      return objectId ? objectId : "Unknown";
    default:
      return "Unknown";
  }
}

static const gchar*
to_str (gchar* cstr)
{
    return reinterpret_cast<const gchar*>(cstr) ? cstr : "";
}

static const gchar *
sensor_id_to_str (NvDsMsg2pCtx *ctx, gint sensorId)
{
  NvDsPayloadPriv *privObj = NULL;
  NvDsSensorObject *dsObj = NULL;

  g_return_val_if_fail (ctx, NULL);
  g_return_val_if_fail (ctx->privData, NULL);

  privObj = (NvDsPayloadPriv *) ctx->privData;

  auto idMap = privObj->sensorObj.find (sensorId);
  if (idMap != privObj->sensorObj.end()) {
    dsObj = &idMap->second;
    return dsObj->id.c_str();
  } else {
    cout << "No entry for " CONFIG_GROUP_SENSOR << sensorId
        << " in configuration file" << endl;
    return NULL;
  }
}

static bool
nvds_msg2p_parse_sensor (NvDsMsg2pCtx *ctx, GKeyFile *key_file, gchar *group)
{
  bool ret = false;
  bool isEnabled = false;
  gchar **keys = NULL;
  gchar **key = NULL;
  GError *error = NULL;
  NvDsPayloadPriv *privObj = NULL;
  NvDsSensorObject sensorObj;
  gint sensorId;
  gchar *keyVal;


  if (sscanf (group, CONFIG_GROUP_SENSOR "%u", &sensorId) < 1) {
    cout << "Wrong sensor group name " << group << endl;
    return ret;
  }

  privObj = (NvDsPayloadPriv *) ctx->privData;

  auto idMap = privObj->sensorObj.find (sensorId);
  if (idMap != privObj->sensorObj.end()) {
    cout << "Duplicate entries for " << group << endl;
    return ret;
  }

  isEnabled = g_key_file_get_boolean (key_file, group, CONFIG_KEY_ENABLE,
                                      &error);
  if (!isEnabled) {
    // Not enabled, skip the parsing of keys.
    ret = true;
    goto done;
  } else {
    g_key_file_remove_key (key_file, group, CONFIG_KEY_ENABLE,
                           &error);
    CHECK_ERROR (error);
  }

  keys = g_key_file_get_keys (key_file, group, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    keyVal = NULL;
    if (!g_strcmp0 (*key, CONFIG_KEY_ID)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_ID, &error);
      sensorObj.id = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_TYPE)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_TYPE, &error);
      sensorObj.type = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_DESCRIPTION)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_DESCRIPTION, &error);
      sensorObj.desc = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_LOCATION)) {
      gsize length;
      gdouble *location = g_key_file_get_double_list (key_file, group,
                                                      CONFIG_KEY_LOCATION,
                                                      &length, &error);
      if (length != 3) {
        cout << "Wrong values provided, it should be like lat;lon;alt" << endl;
        g_free (location);
        goto done;
      }

      memcpy (sensorObj.location, location, length * sizeof (gdouble));
      g_free (location);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_COORDINATE)) {
      gsize length;
      gdouble *coordinate = g_key_file_get_double_list (key_file, group,
                                                      CONFIG_KEY_COORDINATE,
                                                      &length, &error);
      if (length != 3) {
        cout << "Wrong values provided, it should be like x;y;z" << endl;
        g_free (coordinate);
        goto done;
      }

      memcpy (sensorObj.coordinate, coordinate, length * sizeof (gdouble));
      g_free (coordinate);
      CHECK_ERROR (error);
    } else {
      cout << "Unknown key " << *key << " for group [" << group <<"]\n";
    }

    if (keyVal)
      g_free (keyVal);
  }

  privObj->sensorObj.insert (make_pair (sensorId, sensorObj));

  ret = true;

done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }

  return ret;
}

static bool
nvds_msg2p_parse_place (NvDsMsg2pCtx *ctx, GKeyFile *key_file, gchar *group)
{
  bool ret = false;
  bool isEnabled = false;
  gchar **keys = NULL;
  gchar **key = NULL;
  GError *error = NULL;
  NvDsPayloadPriv *privObj = NULL;
  NvDsPlaceObject placeObj;
  gint placeId;
  gchar *keyVal;

  if (sscanf (group, CONFIG_GROUP_PLACE "%u", &placeId) < 1) {
    cout << "Wrong place group name " << group << endl;
    return ret;
  }

  privObj = (NvDsPayloadPriv *) ctx->privData;

  auto idMap = privObj->placeObj.find (placeId);
  if (idMap != privObj->placeObj.end()) {
    cout << "Duplicate entries for " << group << endl;
    return ret;
  }

  isEnabled = g_key_file_get_boolean (key_file, group, CONFIG_KEY_ENABLE,
                                      &error);
  if (!isEnabled) {
    // Not enabled, skip the parsing of keys.
    ret = true;
    goto done;
  } else {
    g_key_file_remove_key (key_file, group, CONFIG_KEY_ENABLE,
                           &error);
    CHECK_ERROR (error);
  }

  keys = g_key_file_get_keys (key_file, group, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    if (!g_strcmp0 (*key, CONFIG_KEY_ID)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_ID, &error);
      placeObj.id = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_TYPE)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_TYPE, &error);
      placeObj.type = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_NAME)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_NAME, &error);
      placeObj.name = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_LOCATION)) {
      gsize length;
      gdouble *location = g_key_file_get_double_list (key_file, group,
                                                      CONFIG_KEY_LOCATION,
                                                      &length, &error);
      if (length != 3) {
        cout << "Wrong values provided, it should be like lat;lon;alt" << endl;
        g_free (location);
        goto done;
      }

      memcpy (placeObj.location, location, length * sizeof (gdouble));
      g_free (location);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_COORDINATE)) {
      gsize length;
      gdouble *coordinate = g_key_file_get_double_list (key_file, group,
                                                      CONFIG_KEY_COORDINATE,
                                                      &length, &error);
      if (length != 3) {
        cout << "Wrong values provided, it should be like x;y;z" << endl;
        g_free (coordinate);
        goto done;
      }

      memcpy (placeObj.coordinate, coordinate, length * sizeof (gdouble));
      g_free (coordinate);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_PLACE_SUB_FIELD1)) {
      keyVal = g_key_file_get_string (key_file, group,
                                        CONFIG_KEY_PLACE_SUB_FIELD1, &error);
      placeObj.subObj.field1 = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_PLACE_SUB_FIELD2)) {
      keyVal = g_key_file_get_string (key_file, group,
                                        CONFIG_KEY_PLACE_SUB_FIELD2, &error);
      placeObj.subObj.field2 = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_PLACE_SUB_FIELD3)) {
      keyVal = g_key_file_get_string (key_file, group,
                                        CONFIG_KEY_PLACE_SUB_FIELD3, &error);
      placeObj.subObj.field3 = keyVal;
      g_free (keyVal);
      CHECK_ERROR (error);
    } else {
      cout << "Unknown key " << *key << " for group [" << group <<"]\n";
    }
  }

  privObj->placeObj.insert (pair<int, NvDsPlaceObject> (placeId, placeObj));

  ret = true;

done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }

  return ret;
}

static bool
nvds_msg2p_parse_analytics (NvDsMsg2pCtx *ctx, GKeyFile *key_file, gchar *group)
{
  bool ret = false;
  bool isEnabled = false;
  gchar **keys = NULL;
  gchar **key = NULL;
  GError *error = NULL;
  NvDsPayloadPriv *privObj = NULL;
  NvDsAnalyticsObject analyticsObj;
  gint moduleId;
  gchar *keyVal;

  if (sscanf (group, CONFIG_GROUP_ANALYTICS "%u", &moduleId) < 1) {
    cout << "Wrong analytics module group name " << group << endl;
    return ret;
  }

  privObj = (NvDsPayloadPriv *) ctx->privData;

  auto idMap = privObj->analyticsObj.find (moduleId);
  if (idMap != privObj->analyticsObj.end()) {
    cout << "Duplicate entries for " << group << endl;
    return ret;
  }

  isEnabled = g_key_file_get_boolean (key_file, group, CONFIG_KEY_ENABLE,
                                      &error);
  if (!isEnabled) {
    // Not enabled, skip the parsing of keys.
    ret = true;
    goto done;
  } else {
    g_key_file_remove_key (key_file, group, CONFIG_KEY_ENABLE,
                           &error);
    CHECK_ERROR (error);
  }

  keys = g_key_file_get_keys (key_file, group, NULL, &error);
  CHECK_ERROR (error);

  for (key = keys; *key; key++) {
    keyVal = NULL;
    if (!g_strcmp0 (*key, CONFIG_KEY_ID)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_ID, &error);
      analyticsObj.id = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_SOURCE)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_SOURCE, &error);
      analyticsObj.source = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_DESCRIPTION)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_DESCRIPTION, &error);
      analyticsObj.desc = keyVal;
      CHECK_ERROR (error);
    } else if (!g_strcmp0 (*key, CONFIG_KEY_VERSION)) {
      keyVal = g_key_file_get_string (key_file, group,
                                      CONFIG_KEY_VERSION, &error);
      analyticsObj.version = keyVal;
      CHECK_ERROR (error);
    } else {
      cout << "Unknown key " << *key << " for group [" << group <<"]\n";
    }

    if (keyVal)
      g_free (keyVal);
  }

  privObj->analyticsObj.insert (make_pair (moduleId, analyticsObj));

  ret = true;

done:
  if (error) {
    g_error_free (error);
  }
  if (keys) {
    g_strfreev (keys);
  }

  return ret;
}

static bool
nvds_msg2p_parse_csv (NvDsMsg2pCtx *ctx, const gchar *file)
{
  NvDsPayloadPriv *privObj = NULL;
  NvDsAnalyticsObject analyticsObj;
  NvDsSensorObject sensorObj;
  NvDsPlaceObject placeObj;
  bool retVal = true;
  bool firstRow = true;
  string line;
  gint i, index = 0;

  ifstream inputFile (file);
  if (!inputFile.is_open()) {
    cout << "Couldn't open CSV file " << file << endl;
    return false;
  }

  privObj = (NvDsPayloadPriv *) ctx->privData;

  try {

    while (getline (inputFile, line)) {

      if (firstRow) {
        // Discard first row as it will have header fields.
        firstRow = false;
        continue;
      }

      vector<string> tokens;
      get_csv_tokens (line, tokens);
      // Ignore first cameraId field.
      i = 1;

      // sensor object fields
      sensorObj.id = tokens.at(i++);
      sensorObj.type = "Camera";
      sensorObj.desc = tokens.at(i++);

      //Hard coded values but can be read from CSV file.
      sensorObj.location[0] = 0; //atof (tokens.at(i++).c_str ());
      sensorObj.location[1] = 0;
      sensorObj.location[2] = 0;
      sensorObj.coordinate[0] = 0;
      sensorObj.coordinate[1] = 0;
      sensorObj.coordinate[2] = 0;

      // place object fields
      placeObj.id = "Id";
      placeObj.type = "building/garage";
      placeObj.name = "endeavor";
      placeObj.location[0] = 0;
      placeObj.location[1] = 0;
      placeObj.location[2] = 0;
      placeObj.coordinate[0] = 0;
      placeObj.coordinate[1] = 0;
      placeObj.coordinate[2] = 0;
      //Ignore cameraIDstring
      i++;
      placeObj.subObj.field1 = tokens.at(i++);
      placeObj.subObj.field2 = tokens.at(i++);
      placeObj.subObj.field3 = tokens.at(i++);

      // analytics object fields
      // hard coded values but can be read from CSV file.
      analyticsObj.id = "";
      analyticsObj.source = "";
      analyticsObj.desc = "";
      analyticsObj.version = "1.0";

      privObj->sensorObj.insert (make_pair (index, sensorObj));
      privObj->placeObj.insert (make_pair (index, placeObj));
      privObj->analyticsObj.insert (make_pair (index, analyticsObj));

      index++;
    }
  } catch (const std::out_of_range& oor) {
    std::cerr << "Out of Range error: " << oor.what() << '\n';
    retVal = false;
  }

  inputFile.close ();
  return retVal;
}

static bool
nvds_msg2p_parse_key_value (NvDsMsg2pCtx *ctx, const gchar *file)
{
  bool retVal = true;
  GKeyFile *cfgFile = NULL;
  GError *error = NULL;
  gchar **groups = NULL;
  gchar **group;

  cfgFile = g_key_file_new ();
  if (!g_key_file_load_from_file (cfgFile, file, G_KEY_FILE_NONE, &error)) {
    g_message ("Failed to load file: %s", error->message);
    retVal = false;
    goto done;
  }

  groups = g_key_file_get_groups (cfgFile, NULL);

  for (group = groups; *group; group++) {
    if (!strncmp (*group, CONFIG_GROUP_SENSOR, strlen (CONFIG_GROUP_SENSOR))) {
      retVal = nvds_msg2p_parse_sensor (ctx, cfgFile, *group);
    } else if (!strncmp (*group, CONFIG_GROUP_PLACE, strlen (CONFIG_GROUP_PLACE))) {
      retVal = nvds_msg2p_parse_place (ctx, cfgFile, *group);
    } else if (!strncmp (*group, CONFIG_GROUP_ANALYTICS, strlen (CONFIG_GROUP_ANALYTICS))) {
      retVal = nvds_msg2p_parse_analytics (ctx, cfgFile, *group);
    } else {
      cout << "Unknown group " << *group << endl;
    }

    if (!retVal) {
      cout << "Failed to parse group " << *group << endl;
      goto done;
    }
  }

done:
  if (groups)
    g_strfreev (groups);

  if (cfgFile)
    g_key_file_free (cfgFile);

  return retVal;
}

NvDsMsg2pCtx* nvds_msg2p_ctx_create (const gchar *file, NvDsPayloadType type)
{
  NvDsMsg2pCtx *ctx = NULL;
  string str;
  bool retVal = true;

  /*
   * Need to parse configuration / CSV files to get static properties of
   * components (e.g. sensor, place etc.) in case of full deepstream schema.
   */
  if (type == NVDS_PAYLOAD_DEEPSTREAM) {
    g_return_val_if_fail (file, NULL);

    ctx = new NvDsMsg2pCtx;
    ctx->privData = (void *) new NvDsPayloadPriv;

    if (g_str_has_suffix (file, ".csv")) {
      retVal = nvds_msg2p_parse_csv (ctx, file);
    } else {
      retVal = nvds_msg2p_parse_key_value (ctx, file);
    }
  } else {
    ctx = new NvDsMsg2pCtx;
    /* If configuration file is provided for minimal schema,
     * parse it for static values.
     */
    if (file) {
      ctx->privData = (void *) new NvDsPayloadPriv;
      retVal = nvds_msg2p_parse_key_value (ctx, file);
    } else {
      ctx->privData = nullptr;
      retVal = true;
    }
  }

  ctx->payloadType = type;

  if (!retVal) {
    cout << "Error in creating instance" << endl;

    if (ctx && ctx->privData)
      delete (NvDsPayloadPriv *) ctx->privData;

    if (ctx) {
      delete ctx;
      ctx = NULL;
    }
  }
  return ctx;
}

void nvds_msg2p_ctx_destroy (NvDsMsg2pCtx *ctx)
{
  delete (NvDsPayloadPriv *) ctx->privData;
  ctx->privData = nullptr;
  delete ctx;
}

NvDsPayload**
nvds_msg2p_generate_multiple (NvDsMsg2pCtx *ctx, NvDsEvent *events, guint eventSize,
                     guint *payloadCount)
{

}

NvDsPayload*
nvds_msg2p_generate (NvDsMsg2pCtx *ctx, NvDsEvent *events, guint size)
{
  gchar *message = NULL;
  gint len = 0;
  NvDsPayload *payload = (NvDsPayload *) g_malloc0 (sizeof (NvDsPayload));

  if (ctx->payloadType == NVDS_PAYLOAD_DEEPSTREAM) {
    message = generate_schema_message (ctx, events->metadata);
    if (message) {
      len = strlen (message);
      // Remove '\0' character at the end of string and just copy the content.
      payload->payload = g_memdup (message, len);
      payload->payloadSize = len;
      g_free (message);
    }
  } 
  return payload;
}

void
nvds_msg2p_release (NvDsMsg2pCtx *ctx, NvDsPayload *payload)
{
  g_free (payload->payload);
  g_free (payload);
}

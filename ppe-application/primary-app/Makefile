# DeepStream
DS_VERSION_MAJOR=5
DS_VERSION_MINOR=0
DS=/opt/nvidia/deepstream/deepstream-$(DS_VERSION_MAJOR).$(DS_VERSION_MINOR)

# Local sources
ifeq ($(LOCAL_SOURCES),0)
	COMMON_DIR:=$(DS)/sources/apps/apps-common
	DSAPP_DIR:=$(DS)/sources/apps/sample_apps/deepstream-app
else
	COMMON_DIR:=apps-common
	DSAPP_DIR:=deepstream-app
endif

# Tegra
TARGET_DEVICE = $(shell gcc -dumpmachine | cut -f1 -d -)
ifeq ($(TARGET_DEVICE),aarch64)
  CFLAGS:= -DPLATFORM_TEGRA
endif

# Dependencies
PKGS:= gstreamer-1.0 gstreamer-video-1.0 x11 json-glib-1.0

# Sources
SRCS:= deepstream_application.c
CPP_SRCS:= generate_event_msg.cpp
SRCS+= $(DSAPP_DIR)/deepstream_app.c $(DSAPP_DIR)/deepstream_app_config_parser.c
SRCS+= $(wildcard $(COMMON_DIR)/src/*.c)
INCS+= $(wildcard $(COMMON_DIR)/includes/*.h)
OBJS:= $(SRCS:.c=.o)
CPP_OBJS:= $(CPP_SRCS:.cpp=.cppo)

# Flags
CFLAGS+=	-I$(COMMON_DIR)/includes -I$(DSAPP_DIR) -I../meta_includes \
			-DDS_VERSION_MAJOR=$(DS_VERSION_MAJOR) -DDS_VERSION_MINOR=$(DS_VERSION_MINOR)
CFLAGS+=$(shell pkg-config --cflags $(PKGS))

# Libs
LIBS+=	-L$(DS)/lib -Wl,-rpath,$(DS)/lib \
		-lnvdsgst_meta -lnvdsgst_helper -lnvdsgst_smartrecord -lnvds_msgbroker \
		-lnvds_meta -lnvds_utils \
		-lgstrtspserver-1.0 \
		-lm -ldl
LIBS+= $(shell pkg-config --libs $(PKGS))

# Rules
APP:= ppe-primary
all: $(APP)
%.o: %.c $(INCS) Makefile
	$(CC) -c -o $@ $(CFLAGS) $<
%.cppo: %.cpp $(INCS) Makefile
	$(CXX) -c -o $@ $(CFLAGS) $<
$(APP): $(OBJS) $(CPP_OBJS) Makefile
	$(CXX) -o $(APP) $(OBJS) $(CPP_OBJS) $(LIBS)
clean:
	rm -rf $(OBJS) $(APP)

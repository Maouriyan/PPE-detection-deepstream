APP_DIR:=primary-app/
SEC_APP_DIR:=secondary-app/
GST_NVMSGCONV_DIR:=gst-nvmsgconv
NVMSGCONV_DIR:=nvmsgconv

all:
	$(MAKE) -C $(APP_DIR) all
	$(MAKE) -C $(SEC_APP_DIR) all
	$(MAKE) -C $(GST_NVMSGCONV_DIR) all
	$(MAKE) -C $(NVMSGCONV_DIR) all

install:
	$(MAKE) -C $(GST_NVMSGCONV_DIR) install
	$(MAKE) -C $(NVMSGCONV_DIR) install
	
clean:
	$(MAKE) -C $(APP_DIR) clean
	$(MAKE) -C $(SEC_APP_DIR) clean
	$(MAKE) -C $(GST_NVMSGCONV_DIR) clean
	$(MAKE) -C $(NVMSGCONV_DIR) clean

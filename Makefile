# Makefile for ProtoPirate Flipper Zero Application

# --- Configuration ---
APP_ID=proto_pirate
FIRMWARE_DIR=unleashed-firmware
APP_SRC_DIR=$(CURDIR)/src
BUILD_DIR=$(CURDIR)/$(FIRMWARE_DIR)/build/f7-firmware-D/.extapps
DIST_DIR=$(CURDIR)/dist
SYMLINK_PATH=$(CURDIR)/$(FIRMWARE_DIR)/applications_user/$(APP_ID)
SYMLINK_DIR=$(CURDIR)/$(FIRMWARE_DIR)/applications_user

# --- Targets ---
.PHONY: all build clean

all: build

build: $(DIST_DIR)/$(APP_ID).fap

# Create a symlink to the application source
$(SYMLINK_PATH):
	@echo "Creating symlink for $(APP_ID)..."
	@mkdir -p $(SYMLINK_DIR)
	@ln -sf $(APP_SRC_DIR) $(SYMLINK_PATH)

# Build the firmware and the application
$(BUILD_DIR)/$(APP_ID).fap: $(SYMLINK_PATH)
	@echo "Building Flipper Application Package (FAP)..."
	@cd $(FIRMWARE_DIR) && ./fbt faps

# Copy the final .fap file to the dist directory
$(DIST_DIR)/$(APP_ID).fap: $(BUILD_DIR)/$(APP_ID).fap
	@echo "Copying FAP to dist directory..."
	@mkdir -p $(DIST_DIR)
	@cp $(BUILD_DIR)/$(APP_ID).fap $(DIST_DIR)/

clean:
	@echo "Cleaning up..."
	@rm -f $(SYMLINK_PATH)
	@rm -rf $(DIST_DIR)
	@cd $(FIRMWARE_DIR) && ./fbt clean

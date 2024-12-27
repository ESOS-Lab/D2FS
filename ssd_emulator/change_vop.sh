#!/bin/bash

# File paths
FILE_PATH2="./nvmev.h"

# Prompt the user for the VIRTUAL_OVP_DEGREE value
read -p "Enter the value for VIRTUAL_OVP_DEGREE (e.g., 1.2): " VIRTUAL_OVP_DEGREE

# Calculate NEW_VALUE by multiplying VIRTUAL_OVP_DEGREE by 10 and rounding
NEW_VALUE=$(echo "$VIRTUAL_OVP_DEGREE * 10" | bc | awk '{print int($1)}')

# Function to update a specific file
update_file() {
  local file=$1
  local pattern=$2
  local new_value=$3

  # Check if the file exists
  if [[ ! -f "$file" ]]; then
    echo "File does not exist: $file"
    return 1
  fi

  # Update the value, matching the exact macro name
  sed -i "s/\(#define\s\+$pattern\s\+\)[0-9]\+/\1$new_value/" "$file"
  echo "$pattern in $file has been updated to $new_value."
}

# Update WINDOW_EXT_RATE in nvmev.h
update_file "$FILE_PATH2" "WINDOW_EXT_RATE" "$NEW_VALUE"


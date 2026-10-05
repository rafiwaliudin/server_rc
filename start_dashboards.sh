#!/bin/bash

# Wait for desktop and docker services to be ready
echo "Waiting for dashboard server on port 8889..."
while ! curl -s http://localhost:8889 > /dev/null; do
    sleep 2
done
sleep 3 # additional buffer just in case

# Clean up old chrome user data to prevent "Restore Session" prompts or lock files
rm -rf /tmp/chrome1 /tmp/chrome2 /tmp/chrome3

# Define monitor width (Change this if your monitors are not 1920px wide, e.g. 2560, 3840)
WIDTH=1920

# Monitor 1 (Steer 1)
google-chrome --no-first-run --no-default-browser-check --disable-infobars --password-store=basic --user-data-dir=/tmp/chrome1 --new-window --window-position=0,0 --kiosk "http://localhost:8889/steering_dashboard.html?steer=0" &
sleep 1

# Monitor 2 (Steer 2)
google-chrome --no-first-run --no-default-browser-check --disable-infobars --password-store=basic --user-data-dir=/tmp/chrome2 --new-window --window-position=${WIDTH},0 --kiosk "http://localhost:8889/steering_dashboard.html?steer=1" &
sleep 1

# Monitor 3 (Steer 3)
google-chrome --no-first-run --no-default-browser-check --disable-infobars --password-store=basic --user-data-dir=/tmp/chrome3 --new-window --window-position=$((WIDTH*2)),0 --kiosk "http://localhost:8889/steering_dashboard.html?steer=2" &

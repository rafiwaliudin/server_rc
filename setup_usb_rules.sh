#!/bin/bash

RULES_FILE="/tmp/99-autopia-steer.rules"
echo "" > $RULES_FILE

echo "=============================================="
echo "    AUTOPIA STEERING WHEEL SYNC SETUP         "
echo "=============================================="
echo "Proses ini akan mengunci Steering Wheel ke USB Port secara permanen."
echo "Pastikan Anda mencolokkan Steering Wheel ke port yang sama seterusnya."
echo ""
echo "LANGKAH 1: CABUT SEMUA STEERING WHEEL DARI NUC!"
read -p "Tekan [Enter] jika semua steer sudah dicabut..."

for i in 1 2 3; do
    echo "----------------------------------------------"
    echo "LANGKAH $((i+1)): COLOKKAN STEERING WHEEL $i SEKARANG!"
    echo "Menunggu perangkat terdeteksi..."
    
    # Wait for a new js device to appear
    NEW_JS=""
    while [ -z "$NEW_JS" ]; do
        NEW_JS=$(ls -1 /dev/input/js* 2>/dev/null | head -n 1)
        sleep 1
    done
    
    echo "Terdeteksi: $NEW_JS"
    
    # Get the parent USB device KERNELS string using udevadm
    # We walk up the sysfs tree until we find the usb device
    USB_KERNELS=$(udevadm info -a -n "$NEW_JS" | grep 'ATTRS{idVendor}' -B 5 | grep 'KERNELS=="[0-9]*-[0-9]*' | head -n 1 | awk -F'==' '{print $2}' | tr -d '"')
    
    if [ -z "$USB_KERNELS" ]; then
        echo "Gagal mendeteksi USB Port. Pastikan itu adalah Steering Wheel."
        exit 1
    fi
    
    echo "Steer $i terkunci di USB Port: $USB_KERNELS"
    
    # Append to rules.
    # We use SYMLINK to create a consistent name, e.g., /dev/input/js_steer_1
    echo "SUBSYSTEM==\"input\", KERNEL==\"js*\", KERNELS==\"$USB_KERNELS\", SYMLINK+=\"input/js_steer_$i\"" >> $RULES_FILE
    
    echo "SEKARANG CABUT KEMBALI STEERING WHEEL $i!"
    read -p "Tekan [Enter] jika sudah dicabut..."
    
    # Wait for it to disappear
    while [ -e "$NEW_JS" ]; do
        sleep 1
    done
done

echo "----------------------------------------------"
echo "Menyimpan konfigurasi ke sistem (butuh password sudo)..."
sudo cp $RULES_FILE /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger

echo "=============================================="
echo "SETUP SELESAI!"
echo "Sekarang Anda bisa mencolokkan ketiga Steering Wheel secara bersamaan."
echo "Mereka akan selalu dikenali secara berurutan sesuai setup tadi."
echo "=============================================="

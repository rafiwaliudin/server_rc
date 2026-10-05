import pygame
import sys
import time

def main():
    pygame.init()
    pygame.joystick.init()

    count = pygame.joystick.get_count()
    if count == 0:
        print("Tidak ada joystick/steering wheel yang terdeteksi.")
        sys.exit()

    print(f"Terdeteksi {count} perangkat.")
    joysticks = []
    for i in range(count):
        joy = pygame.joystick.Joystick(i)
        joy.init()
        joysticks.append(joy)
        print(f"[{i}] {joy.get_name()} (Axes: {joy.get_numaxes()}, Buttons: {joy.get_numbuttons()})")

    print("\nSilakan tekan pedal gas, rem, dan tombol-tombol pada controller Anda.")
    print("Perhatikan angka Axis dan Button mana yang berubah di layar.")
    print("Tekan CTRL+C untuk berhenti.\n")

    try:
        while True:
            pygame.event.pump()
            for i, joy in enumerate(joysticks):
                axes = [round(joy.get_axis(a), 2) for a in range(joy.get_numaxes())]
                
                buttons = []
                for b in range(joy.get_numbuttons()):
                    if joy.get_button(b):
                        buttons.append(b)
                
                print(f"\rDevice {i} | Axes: {axes} | Pressed Buttons: {buttons}      ", end="")
            time.sleep(0.1)
    except KeyboardInterrupt:
        print("\n\nSelesai. Silakan sesuaikan wheel_config.yaml dengan Axis yang tepat.")
        pygame.quit()

if __name__ == '__main__':
    main()

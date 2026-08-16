import subprocess
import time

pcs = []
for _ in range(50):
    output = subprocess.check_output(['arm-none-eabi-gdb', '-batch', '-ex', 'target extended-remote localhost:3333', '-ex', 'info registers pc'])
    pc_line = [line for line in output.decode().split('\n') if line.startswith('pc ')]
    if pc_line:
        pc_hex = pc_line[0].split()[1]
        pcs.append(pc_hex)
    time.sleep(0.05)

for pc in pcs:
    print(pc)

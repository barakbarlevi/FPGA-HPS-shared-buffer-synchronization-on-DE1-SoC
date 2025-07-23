# FPGA HPS shared buffer synchronization on DE1-SoC


We’ll have the FPGA (Cyclone V) write data to a 1MB ring buffer. Its writing may pause for an arbitrary amount of time, simulating it waiting on some other tasks to finish. A C program running on the HPS (which includes an ARM Cortex-A9 processor) will flawlessly retrieve data from the buffer and process it. No misses, no duplications.

![Image](https://github.com/user-attachments/assets/600ebd04-4714-4e03-b25b-48eb92cc1341)

See the full documentation [here](https://docs.google.com/document/d/1mpWA9rT2MACNfZER9HTnCref5cQrCMWCV_RoO3sbMvU/edit?usp=sharing)  
It It draws from [this document](https://drive.google.com/file/d/1myHPIgDS3YTJ36jgYJsa536rMOdVYWa2/view?usp=drive_link) I wrote on the basics of FPGA design

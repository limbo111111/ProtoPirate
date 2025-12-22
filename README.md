# **ProtoPirate**

### _for Flipper Zero_

ProtoPirate is an experimental rolling-code analysis toolkit developed by members of **The Pirates' Plunder**.
The app currently supports decoding for multiple automotive key-fob families (Kia, Ford, Subaru, Suzuki, VW, and more), with the goal of being a drop-in Flipper app (.fap) that is free, open source, and can be used on any Flipper Zero firmware.

## **Supported Protocols**

Decoders:

- KIA V0
- KIA V1
- KIA V2
- KIA V3 / V4
- KIA V5
- Ford V0
- Subaru
- Suzuki
- Volkswagen (VW)

Encoders: **Coming Soon**
- KIA V0
  
---

## **Credits**

The following contributors are recognized for helping us keep open sourced projects and the freeware community alive.

In alphabetical order 😎

### **Protocol Magic**

- DoobTheGoober
- L0rdDiakon
- RocketGod
- Skorp
- Slackware
- Trikk
- Wootini
- YougZ

### **App Development**

- MMX
- RocketGod
- Skorp - Thanks, I sneaked a lot from Weather App!
- Vadim's Radio Driver

### **Reverse Engineering Support**

- DoobTheGoober
- MMX
- NeedNotApply
- RocketGod
- Slackware
- Trikk

---

## **Community & Support**

Join **The Pirates' Plunder** on Discord for development updates, testing, protocol research, community support, and a bunch of badasses doing fun shit:

➡️ **[https://discord.gg/thepirates](https://discord.gg/thepirates)**

<img width="1500" height="1000" alt="rocketgod_logo_transparent" src="https://github.com/user-attachments/assets/ad15b106-152c-4a60-a9e2-4d40dfa8f3c6" />

---

## **Development Reference**

The `reference/` directory contains code and data that may be useful for future development of encoders.

**IMPORTANT:** The C code in this directory is **not functional** and should not be integrated into the application without significant modification. It contains a flawed Keeloq implementation that is missing the necessary key derivation step. The manufacturer keys and protocol structures may still be useful as a starting point for a correct implementation.

# Research Report: Missing Encoders

## Overview
This report analyzes the current state of encoders in the Protopirate application and identifies missing encoders based on the provided reference materials.

## Implemented Protocols
The following protocols have functional encoders implemented in the codebase:

| Protocol Name | File | Status | Notes |
|---|---|---|---|
| Ford V0 | `protocols/ford_v0.c` | Implemented | Decoder and Encoder present. |
| Kia V0 | `protocols/kia_v0.c` | Implemented | Decoder and Encoder present. |
| Kia V1 | `protocols/kia_v1.c` | Implemented | Decoder and Encoder present. |
| Kia V2 | `protocols/kia_v2.c` | Implemented | Decoder and Encoder present. |
| Kia V3/V4 | `protocols/kia_v3_v4.c` | Implemented | Decoder and Encoder present. Uses KeeLoq. |
| Kia V5 | `protocols/kia_v5.c` | Implemented | Decoder and Encoder present. |
| Subaru | `protocols/subaru.c` | Implemented | Decoder and Encoder present. |
| Suzuki | `protocols/suzuki.c` | Implemented | Decoder and Encoder present. |
| VW | `protocols/vw.c` | Implemented | Decoder and Encoder present. |

All implemented protocols have the `SubGhzProtocolFlag_Send` flag set and contain `yield` functions with logic (state machines or bit processing).

## Missing Protocols (from Reference)
The file `reference/cars.txt` lists several car models and protocols that are **not** currently implemented in the application.

**Note:** The data in `reference/cars.txt` (keys, hop codes) appears to be placeholder/dummy data (e.g., sequential hex values like `0x1A2B3C...`), which indicates that valid cryptographic keys and algorithms for these protocols are likely not yet available or integrated.

### List of Missing Protocols
1.  **Megamos Crypto / Megamos**
    *   **Cars:** Volkswagen Golf 8, Honda CR-V, Audi Q5, Skoda Kodiaq, Land Rover Defender, Opel Corsa, Kia Sportage (note: Kia is usually KeeLoq, but listed as Megamos here), Mazda CX-5.
    *   **Status:** Missing. Requires Megamos Crypto algorithm implementation.
2.  **Hitag3**
    *   **Cars:** BMW iX, Jaguar I-PACE.
    *   **Status:** Missing.
3.  **AES**
    *   **Cars:** Mercedes S-Class, Porsche Taycan, Tesla Model 3, Lexus RX.
    *   **Status:** Missing. Requires AES crypto library and specific key derivation logic.
4.  **DST+ (Digital Signature Transponder)**
    *   **Cars:** Ford Mustang, Volvo XC90.
    *   **Status:** Missing.
5.  **Hitag2**
    *   **Cars:** Toyota RAV4, Hyundai Tucson, Subaru Forester.
    *   **Status:** Missing.
6.  **DST (40/80 bit)**
    *   **Cars:** Nissan Leaf, Renault Clio, Fiat 500, Chevrolet Bolt, Mitsubishi Outlander.
    *   **Status:** Missing.
7.  **Hitag**
    *   **Cars:** Seat Ateca, Peugeot 3008, Jeep Wrangler.
    *   **Status:** Missing.

## Conclusion
While the core supported protocols (Ford, Kia, Subaru, Suzuki, VW) are fully implemented with encoders, there is a significant gap between the supported list and the "wishlist" in `reference/cars.txt`.

To implement the missing encoders, the following is required:
1.  **Valid Algorithms:** Reverse-engineered logic for protocols like Megamos Crypto, Hitag2/3, DST+, etc.
2.  **Valid Keys:** Real manufacturer keys (not placeholders).
3.  **Protocol Definitions:** New protocol files (header/source) for each family.

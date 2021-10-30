# Dynamic Taint Analysis with libdft

## Index

  - [Overview](#overview) 
  - [Getting Started](#getting-started)
  - [Usage](#Usage)

## Overview

- A simple example code about **DTA(Dynamic Tiant Analysis)**.
- This codes are based on Practical Binary Analysis.
- It was implemented using **libdft** and **paintool**.

## Getting Started

### Dependencies

- pin-2.13

```
tar -zvxf pin-2.13.tar.gz
rm pin-2.13.tar.gz
```

- gcc-multilib

```
sudo apt-get install gcc-multilib g++-multilib
```

- specific kernel: `linux-headers-3.19.0-31_3.19.0-31.36~14.04.1_all.deb`

## Usage

Usage: `${PIN-SH} -follow_execv -t ${DTA-OBJ} -- ${BIN}`

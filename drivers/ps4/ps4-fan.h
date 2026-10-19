/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _PS4_FAN_H
#define _PS4_FAN_H

#define PS4_FAN_ICC_MAJOR               0x0A
#define PS4_FAN_ICC_MINOR_GET           0x07
#define PS4_FAN_ICC_MINOR_SET           0x06

#define PS4_FAN_TEMP_ICC_MAJOR          0x0B
#define PS4_FAN_TEMP_ICC_MINOR          0x01

#define PS4_FAN_CONFIG_LEN              0x34
#define PS4_FAN_CONFIG_REPLY_LEN        0x52
#define PS4_FAN_TEMP_REPLY_LEN          0x10

#define PS4_ICC_STATUS_BYTE             0
#define PS4_FAN_THRESH_BYTE             5
#define PS4_FAN_TEMP_BYTE               3

#define PS4_FAN_THRESH_MIN_C            20
#define PS4_FAN_THRESH_MAX_C            85
#define PS4_FAN_THRESH_DEFAULT_C        75

#define PS4_FAN_THRESH_MIN_MC           ((long)(PS4_FAN_THRESH_MIN_C) * 1000L)
#define PS4_FAN_THRESH_MAX_MC           ((long)(PS4_FAN_THRESH_MAX_C) * 1000L)
#define PS4_FAN_THRESH_DEFAULT_MC       ((long)(PS4_FAN_THRESH_DEFAULT_C) * 1000L)

#endif /* _PS4_FAN_H */

/*
 * Copyright © 2024 GNOME Foundation, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *       Hubert Figuière <hub@figuiere.net>
 */

#pragma once

#include <stdint.h>

typedef enum
{
  FLATPAK_USB_RULE_TYPE_ALL,
  FLATPAK_USB_RULE_TYPE_CLASS,
  FLATPAK_USB_RULE_TYPE_DEVICE,
  FLATPAK_USB_RULE_TYPE_VENDOR,
} UsbRuleType;

typedef enum
{
  FLATPAK_USB_RULE_CLASS_TYPE_CLASS_ONLY,
  FLATPAK_USB_RULE_CLASS_TYPE_CLASS_SUBCLASS,
} UsbDeviceClassType;

typedef struct
{
  UsbDeviceClassType type;
  uint16_t class;
  uint16_t subclass;
} UsbDeviceClass;

typedef struct
{
  uint16_t id;
} UsbProduct;

typedef struct
{
  uint16_t id;
} UsbVendor;

typedef struct
{
  UsbRuleType rule_type;

  union {
    UsbDeviceClass device_class;
    UsbProduct product;
    UsbVendor vendor;
  } d;
} FlatpakUsbRule;

typedef struct
{
  GPtrArray *rules;
} FlatpakUsbQuery;


void flatpak_usb_rule_print (FlatpakUsbRule *usb_rule,
                             GString        *string);
void flatpak_usb_rule_free (FlatpakUsbRule *usb_rule);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakUsbRule, flatpak_usb_rule_free)

FlatpakUsbQuery *flatpak_usb_query_new (void);
FlatpakUsbQuery *flatpak_usb_query_copy (const FlatpakUsbQuery *query);
void flatpak_usb_query_print (const FlatpakUsbQuery *usb_query,
                              GString               *string);
void flatpak_usb_query_free (FlatpakUsbQuery *usb_query);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlatpakUsbQuery, flatpak_usb_query_free)

gboolean flatpak_usb_parse_usb_rule (const char      *data,
                                     FlatpakUsbRule **out_usb_rule,
                                     GError         **error);
gboolean flatpak_usb_parse_usb (const char       *data,
                                FlatpakUsbQuery **out_usb_query,
                                GError          **error);


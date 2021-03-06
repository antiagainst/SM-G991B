/*
 * Copyright (c) 2020-2021 Samsung Electronics Co., Ltd. All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef _DSMS_MESSAGE_LIST_H
#define _DSMS_MESSAGE_LIST_H

#include "dsms_test.h"

#define LIST_COUNT_LIMIT (50)

extern int __kunit_init dsms_message_list_init(void);
extern void __kunit_exit dsms_message_list_exit(void);

extern struct dsms_message *get_dsms_message(void);
extern int process_dsms_message(struct dsms_message *message);
extern int dsms_check_message_list_limit(void);

#endif /* _DSMS_MESSAGE_LIST_H */

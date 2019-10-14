/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <00pauln00@gmail.com> 2019
 */

#include <uuid/uuid.h>

#include "common.h"

void
niosd_uuid_generate(niosd_id_t *niosd_id)
{
    uuid_generate(niosd_id->nosd_uuid);
}
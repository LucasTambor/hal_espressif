// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/lock.h>

#include "esp_attr.h"
#include "esp_flash_data_types.h"
#include "esp_spi_flash.h"
#include "esp_partition.h"
#include "esp_log.h"


#ifndef NDEBUG
// Enable built-in checks in queue.h in debug builds
#define INVARIANTS
#endif
#include "rom/queue.h"


typedef struct partition_list_item_ {
    esp_partition_t info;
    SLIST_ENTRY(partition_list_item_) next;
} partition_list_item_t;

typedef struct esp_partition_iterator_opaque_ {
    esp_partition_type_t type;                  // requested type
    const char* label;                          // requested label (can be NULL)
    partition_list_item_t* next_item;     // next item to iterate to
    esp_partition_t* info;                // pointer to info (it is redundant, but makes code more readable)
} esp_partition_iterator_opaque_t;


static esp_partition_iterator_opaque_t* iterator_create(esp_partition_type_t type, const char* label);
static esp_err_t load_partitions();


static SLIST_HEAD(partition_list_head_, partition_list_item_) s_partition_list =
        SLIST_HEAD_INITIALIZER(s_partition_list);
static _lock_t s_partition_list_lock;


static uint32_t get_major_type(esp_partition_type_t type)
{
    return (type >> 8) & 0xff;
}

static uint32_t get_minor_type(esp_partition_type_t type)
{
    return type & 0xff;
}

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
        const char* label)
{
    if (SLIST_EMPTY(&s_partition_list)) {
        // only lock if list is empty (and check again after acquiring lock)
        _lock_acquire(&s_partition_list_lock);
        esp_err_t err = ESP_OK;
        if (SLIST_EMPTY(&s_partition_list)) {
            err = load_partitions();
        }
        _lock_release(&s_partition_list_lock);
        if (err != ESP_OK) {
            return NULL;
        }
    }
    // create an iterator pointing to the start of the list
    // (next item will be the first one)
    esp_partition_iterator_t it = iterator_create(type, label);
    // advance iterator to the next item which matches constraints
    it = esp_partition_next(it);
    // if nothing found, it == NULL and iterator has been released
    return it;
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{
    assert(it);
    // iterator reached the end of linked list?
    if (it->next_item == NULL) {
        return NULL;
    }
    uint32_t requested_major_type = get_major_type(it->type);
    uint32_t requested_minor_type = get_minor_type(it->type);
    _lock_acquire(&s_partition_list_lock);
    for (; it->next_item != NULL; it->next_item = SLIST_NEXT(it->next_item, next)) {
        esp_partition_t* p = &it->next_item->info;
        uint32_t it_major_type = get_major_type(p->type);
        uint32_t it_minor_type = get_minor_type(p->type);
        if (requested_major_type != it_major_type) {
            continue;
        }
        if (requested_minor_type != 0xff && requested_minor_type != it_minor_type) {
            continue;
        }
        if (it->label != NULL && strcmp(it->label, p->label) != 0) {
            continue;
        }
        // all constraints match, bail out
        break;
    }
    _lock_release(&s_partition_list_lock);
    if (it->next_item == NULL) {
        esp_partition_iterator_release(it);
        return NULL;
    }
    it->info = &it->next_item->info;
    it->next_item = SLIST_NEXT(it->next_item, next);
    return it;
}

const esp_partition_t* esp_partition_find_first(esp_partition_type_t type, const char* label)
{
    esp_partition_iterator_t it = esp_partition_find(type, label);
    if (it == NULL) {
        return NULL;
    }
    const esp_partition_t* res = esp_partition_get(it);
    esp_partition_iterator_release(it);
    return res;
}

void esp_partition_iterator_release(esp_partition_iterator_t iterator)
{
    // iterator == NULL is okay
    free(iterator);
}

const esp_partition_t* esp_partition_get(esp_partition_iterator_t iterator)
{
    assert(iterator != NULL);
    return iterator->info;
}

esp_partition_type_t esp_partition_type(esp_partition_iterator_t iterator)
{
    return esp_partition_get(iterator)->type;
}

uint32_t esp_partition_size(esp_partition_iterator_t iterator)
{
    return esp_partition_get(iterator)->size;
}

uint32_t esp_partition_address(esp_partition_iterator_t iterator)
{
    return esp_partition_get(iterator)->address;
}

const char* esp_partition_label(esp_partition_iterator_t iterator)
{
    return esp_partition_get(iterator)->label;
}

static esp_partition_iterator_opaque_t* iterator_create(esp_partition_type_t type, const char* label)
{
    esp_partition_iterator_opaque_t* it =
            (esp_partition_iterator_opaque_t*) malloc(sizeof(esp_partition_iterator_opaque_t));
    it->type = type;
    it->label = label;
    it->next_item = SLIST_FIRST(&s_partition_list);
    it->info = NULL;
    return it;
}

// Create linked list of partition_list_item_t structures.
// This function is called only once, with s_partition_list_lock taken.
static esp_err_t load_partitions()
{
    const uint32_t* ptr;
    spi_flash_mmap_handle_t handle;
    // map 64kB block where partition table is located
    esp_err_t err = spi_flash_mmap(ESP_PARTITION_TABLE_ADDR & 0xffff0000,
            SPI_FLASH_SEC_SIZE, SPI_FLASH_MMAP_DATA, (const void**) &ptr, &handle);
    if (err != ESP_OK) {
        return err;
    }
    // calculate partition address within mmap-ed region
    const esp_partition_info_t* it = (const esp_partition_info_t*)
            (ptr + (ESP_PARTITION_TABLE_ADDR & 0xffff) / sizeof(*ptr));
    const esp_partition_info_t* end = it + SPI_FLASH_SEC_SIZE / sizeof(*it);
    // tail of the linked list of partitions
    partition_list_item_t* last = NULL;
    for (; it != end; ++it) {
        if (it->magic != ESP_PARTITION_MAGIC) {
            break;
        }
        // allocate new linked list item and populate it with data from partition table
        partition_list_item_t* item = (partition_list_item_t*) malloc(sizeof(partition_list_item_t));
        item->info.address = it->pos.offset;
        item->info.size = it->pos.size;
        item->info.type = (it->type << 8) | it->subtype;
        item->info.encrypted = false;
        // it->label may not be zero-terminated
        strncpy(item->info.label, (const char*) it->label, sizeof(it->label));
        item->info.label[sizeof(it->label)] = 0;
        // add it to the list
        if (last == NULL) {
            SLIST_INSERT_HEAD(&s_partition_list, item, next);
        } else {
            SLIST_INSERT_AFTER(last, item, next);
        }
    }
    spi_flash_munmap(handle);
    return ESP_OK;
}

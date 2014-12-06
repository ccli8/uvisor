#ifndef __UVISOR_LIB_H__
#define __UVISOR_LIB_H__

#ifdef __cplusplus
#define UVISOR_EXTERN extern "C"
#else
#define UVISOR_EXTERN extern
#endif

#define UVISOR_BOX_MAGIC 0x42CFB66FUL
#define UVISOR_BOX_VERSION 100

#define UVISOR_PACKED __attribute__((packed))
#define UVISOR_SET_MODE(mode) UVISOR_EXTERN const uint32_t __uvisor_mode = (mode);
#define UVISOR_ARRAY_COUNT(x) (sizeof(x)/sizeof(x[0]))
#define UVISOR_ROUND32(x) (((x)+31UL) & ~0x1FUL)

#ifndef/*UVISOR_BOX_STACK*/
#define  UVISOR_BOX_STACK 1024
#endif /*UVISOR_BOX_STACK*/

#define UVISOR_BOX_CONFIG(config_type) \
    __attribute__((section(".text.secured"), aligned(32))) \
    static const volatile struct { \
        config_type secure; \
        uint8_t padding[UVISOR_ROUND32(config_type) - sizeof(config_type)]; \
    } UVISOR_PACKED __uvisor_box_settings =

typedef union
{
    uint32_t value;
} UVISOR_PACKED UvBoxAcl;

typedef struct
{
    const void* start;
    uint32_t length;
    UvBoxAcl acl;
} UVISOR_PACKED UvBoxAclItem;

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t stack_size;

    const UvBoxAclItem* const acl_list;
    uint32_t acl_count;

    const void** const fn_list;
    uint32_t fn_count;
    uint32_t reserved;
} UVISOR_PACKED UvBoxConfig;

#endif/*__UVISOR_LIB_H__*/
#ifndef SM4_CTR128_H
#define SM4_CTR128_H
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"{
#endif
    const unsigned char _key16[] = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
                                       0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f}; // 密钥
    /* 初始化密钥 */
    int SM4_init_key(const char *key);

    /* 国密 CTR 流式加解密 */
    void SM4_ctr128_encrypt(const uint8_t *in, uint8_t *out, size_t sz, off_t offset);

    void SM4_ctr128_decrypt(const uint8_t *in, uint8_t *out, size_t sz, off_t offset);

#ifdef __cplusplus
}
#endif


#endif

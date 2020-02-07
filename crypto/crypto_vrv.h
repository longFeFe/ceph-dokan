#ifndef SM4_CTR128_H
#define SM4_CTR128_H
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"{
#endif

    /* 初始化密钥 */
    int SM4_init_key(const char *key);

    /* 国密 CTR 流式加解密 */
    void SM4_ctr128_encrypt(const uint8_t *in, uint8_t *out, size_t sz, off_t offset);

    void SM4_ctr128_decrypt(const uint8_t *in, uint8_t *out, size_t sz, off_t offset);

#ifdef __cplusplus
}
#endif


#endif

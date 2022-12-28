#include "esp_wb.hpp"
#include "wifibroadcast.hpp"
#include "esp_private/wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_adc_cal.h"
#include "esp_wifi_types.h"
#include "esp_heap_caps.h"
#include "endian.h"
#include "fec.h"
#include "ieee80211_radiotap.h"

#include "key.h"

#include <algorithm>

#include "esp_camera.h"
//#include "EEPROM.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include <driver/adc.h>
#include "esp_adc_cal.h"
#include "esp_wifi_types.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
//#include "esp_wifi_internal.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_private/wifi.h"
#include "esp_task_wdt.h"
//#include "bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "sodium.h"
#include "esp_wb.hpp"
#include "fec.h"

#include "driver/gpio.h"
#include "main.h"
#include "queue.h"
#include "packets.h"
#include "safe_printf.h"
#include "structures.h"
#include "crc.h"
#include "circular_buffer.h"

#define LOG(...) do { SAFE_PRINTF(__VA_ARGS__); } while (false) 
#define max(a,b) (a>b?a:b)


Transmitter::Transmitter(int k, int n, uint8_t radio_port):  fec_k(k), fec_n(n), block_idx(0),
                                                                fragment_idx(0),
                                                                max_packet_size(0),
                                                                radio_port(radio_port)
{
    fec_p = fec_new(fec_k, fec_n);

    block = new uint8_t*[fec_n];
    for(int i=0; i < fec_n; i++)
    {
        block[i] = new uint8_t[MAX_FEC_PAYLOAD];
    }

    make_session_key();
}

Transmitter::~Transmitter()
{
    for(int i=0; i < fec_n; i++)
    {
        delete block[i];
    }
    delete block;

    fec_free(fec_p);
}


void Transmitter::make_session_key(void)
{
    randombytes_buf(session_key, sizeof(session_key));
    session_key_packet.packet_type = WFB_PACKET_KEY;
    randombytes_buf(session_key_packet.session_key_nonce, sizeof(session_key_packet.session_key_nonce));
    if (crypto_box_easy(session_key_packet.session_key_data, session_key, sizeof(session_key),
                        session_key_packet.session_key_nonce, (uint8_t *)rx_pubkey, (uint8_t *)tx_secretkey) != 0)
    {
        //throw runtime_error("Unable to make session key!");
        ;
    }
}

void  Transmitter::inject_packet(const uint8_t *buf, size_t size)
{

    uint8_t *p = txbuf;

    // radiotap header
    //memcpy(p, radiotap_header, sizeof(radiotap_header));     // may be optimized later
    //p += sizeof(radiotap_header);

    // ieee80211 header
    memcpy(p, ieee80211_header, sizeof(ieee80211_header));
    p[SRC_MAC_LASTBYTE] = radio_port;
    p[DST_MAC_LASTBYTE] = radio_port;
    p[FRAME_SEQ_LB] = ieee80211_seq & 0xff;
    p[FRAME_SEQ_HB] = (ieee80211_seq >> 8) & 0xff;
    ieee80211_seq += 16;
    p += sizeof(ieee80211_header);

    // FEC data
    memcpy(p, buf, size);
    p += size;
    esp_wifi_80211_tx(WIFI_IF_STA, txbuf,p - txbuf, false);

}


void Transmitter::send_block_fragment(size_t packet_size)
{
    wblock_hdr_t *block_hdr = (wblock_hdr_t*)ciphertext;
    long long unsigned int ciphertext_len;

    //assert(packet_size <= MAX_FEC_PAYLOAD);

    block_hdr->packet_type = WFB_PACKET_DATA;
    block_hdr->nonce = htobe64(((block_idx & BLOCK_IDX_MASK) << 8) + fragment_idx);

    LOG("block_idx:%lld   fragment_idx:%d\n",block_idx,fragment_idx);

    // encrypted payload
    // encrypt 1000 bytes ~= 400 us
    TEST_TIME_FUNC(crypto_aead_chacha20poly1305_encrypt(ciphertext + sizeof(wblock_hdr_t), &ciphertext_len,
                                         block[fragment_idx], packet_size,
                                         (uint8_t*)block_hdr, sizeof(wblock_hdr_t),
                                         NULL, (uint8_t*)(&(block_hdr->nonce)), session_key), ENCRYPT);


    //inject_packet 1000 bytes  ~= 180 us
    TEST_TIME_FUNC(inject_packet(ciphertext, sizeof(wblock_hdr_t) + ciphertext_len),INJECT);

 
    
}

void Transmitter::send_session_key(void)
{
    //fprintf(stderr, "Announce session key\n");
    inject_packet((uint8_t*)&session_key_packet, sizeof(session_key_packet));
}

uint8_t * Transmitter::push(size_t size){
    wpacket_hdr_t packet_hdr;

    if(push_fragment_idx >= fec_n){
        return ;
    }
    packet_hdr.packet_size = htobe16(size);

    memset(block[push_fragment_idx], 0, MAX_FEC_PAYLOAD);
    memcpy(block[push_fragment_idx], &packet_hdr, sizeof(packet_hdr));
    //memcpy(block[push_fragment_idx] + sizeof(packet_hdr), buf, size);

    //push_fragment_idx++; 
    return &block[push_fragment_idx++];
}


void Transmitter::send_packet(const uint8_t *buf, size_t size, uint8_t flags)
{
    wpacket_hdr_t packet_hdr;
    //assert(size <= MAX_PAYLOAD_SIZE);

    // FEC-only packets are only for closing already opened blocks
    if(fragment_idx == 0 && flags & WFB_PACKET_FEC_ONLY)
    {
        return;
    }

    packet_hdr.packet_size = htobe16(size);
    packet_hdr.flags = flags;
    memset(block[fragment_idx], '\0', MAX_FEC_PAYLOAD);
    memcpy(block[fragment_idx], &packet_hdr, sizeof(packet_hdr));
    memcpy(block[fragment_idx] + sizeof(packet_hdr), buf, size);


    send_block_fragment(sizeof(packet_hdr) + size);


    max_packet_size = max(max_packet_size, sizeof(packet_hdr) + size);
    fragment_idx += 1;

    if (fragment_idx < fec_k)  return;



    // fec 4000 bytes (4 blocks, 1000bytes per block) ~= 900 us
    TEST_TIME_FUNC(fec_encode(fec_p, (const uint8_t**)block, block + fec_k, max_packet_size),FEC);
    while (fragment_idx < fec_n)
    {
        send_block_fragment(max_packet_size);
        fragment_idx += 1;
    }
    block_idx += 1;
    fragment_idx = 0;
    max_packet_size = 0;


    // Generate new session key after MAX_BLOCK_IDX blocks
    if (block_idx > MAX_BLOCK_IDX)
    {
        make_session_key();
        send_session_key();
        block_idx = 0;
    }
}




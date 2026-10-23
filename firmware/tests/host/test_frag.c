/* Host-side test for yd_frag (fragmentation / reassembly over the BLE link). Run: make -C firmware/tests/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yd_link.h"
#include "yd_frag.h"
static uint8_t frags[400][256]; static uint16_t flen[400]; static int nf; static uint16_t maxp=222;
esp_err_t yd_link_send(uint8_t ch, const uint8_t *d, uint16_t l){ if(l>maxp) {printf("TOO BIG %u\n",l); exit(1);} memcpy(frags[nf],d,l); flen[nf++]=l; return ESP_OK; }
uint16_t yd_link_max_payload(void){return maxp;}
int main(){
  static uint8_t msg[3136], buf[3136]; yd_frag_rx_t r; yd_frag_rx_init(&r, buf, sizeof buf);
  yd_frag_init();
  int ok=0;
  for (int it=0; it<3000; it++){
    maxp = (it%3==0)?222:(it%3==1?60:30);
    size_t len = 1 + rand()%3136; for(size_t i=0;i<len;i++) msg[i]=rand();
    nf=0; if (yd_frag_send(3,msg,len)!=ESP_OK){printf("send fail len=%zu maxp=%u\n",len,maxp); continue;}
    int drop = (it%7==0 && nf>2) ? 1+rand()%(nf-1) : -1;
    int got=0;
    for(int i=0;i<nf;i++){ if(i==drop) continue; int n=yd_frag_rx_feed(&r,frags[i],flen[i]); if(n>0){ got=n; } }
    if (drop<0){ if(got!=(int)len||memcmp(buf,msg,len)){printf("MISMATCH it=%d\n",it);return 1;} ok++; }
    else if (got>0){ printf("delivered despite drop it=%d\n",it); return 1; }
  }
  /* START arriving mid-message resets */
  nf=0; maxp=222; yd_frag_send(3,msg,1000); int half=nf/2; for(int i=0;i<half;i++) yd_frag_rx_feed(&r,frags[i],flen[i]);
  int nf1=nf; yd_frag_send(3,msg,500); int got=0; for(int i=nf1;i<nf;i++){int n=yd_frag_rx_feed(&r,frags[i],flen[i]); if(n>0)got=n;}
  printf("ok=%d restart_got=%d dropped=%u\n", ok, got, r.dropped);
  return got==500?0:1;
}

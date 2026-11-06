/* Master application (node 1). Stage 1: empty main. */
#if NODE_ID != 1
#error "app_master.c is for node 1 only"
#endif

int main(void)
{
    for (;;) {
    }
}

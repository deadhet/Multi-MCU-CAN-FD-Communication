/* Slave application (nodes 2-4). Stage 1: empty main. */
#if NODE_ID < 2 || NODE_ID > 4
#error "app_slave.c is for nodes 2-4 only"
#endif

int main(void)
{
    for (;;) {
    }
}

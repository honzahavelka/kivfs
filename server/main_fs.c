/*
 * kivfs-fs – modul FS (kap. 7.1). Zatím jen kostra: propojení
 * s ostatními moduly, HELLO, PING, STATUS (viz module.h).
 * Použití: kivfs-fs -c <soubor.ini>
 */
#include "module.h"

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = { .self = KIVFS_MOD_FS };
    return kivfs_module_main(argc, argv, &ops);
}

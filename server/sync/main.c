/*
 * kivfs-sync – modul SYNC (kap. 7.1). Zatím jen kostra: propojení
 * s ostatními moduly, HELLO, PING, STATUS (viz module.h).
 * Použití: kivfs-sync -c <soubor.ini>
 */
#include "core/module.h"

int main(int argc, char **argv)
{
    static const struct kivfs_mod_ops ops = { .self = KIVFS_MOD_SYNC };
    return kivfs_module_main(argc, argv, &ops);
}

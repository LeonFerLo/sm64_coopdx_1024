#ifndef SMLUA_SYNC_TABLE_H
#define SMLUA_SYNC_TABLE_H

#define MAX_UNWOUND_LNT 16

enum LuaSyncTableType {
    LST_NORMAL,
    LST_PLAYER,
    LST_MAX,
};

struct LSTNetworkType;
void smlua_set_sync_table_field_from_network(u64 seq, u16 modRemoteIndex, u16 lntKeyCount, struct LSTNetworkType* lntKeys, struct LSTNetworkType* lntValue);
void smlua_sync_table_init_globals(const char* path, u16 remoteIndex);
void smlua_bind_sync_table(void);
void smlua_sync_table_send_all(u8 toLocalIndex);
#ifdef SMNET
void smlua_sync_table_send_own_player(void);
void smlua_sync_table_send_own_global(void);
void smlua_sync_table_clear_player(u8 localIndex);
int smlua_mod_index_from_path(const char* path);
const char* smlua_mod_path_from_index(int index);
#endif

#endif
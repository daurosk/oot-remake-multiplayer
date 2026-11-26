/* Offline check of the drop-in's symbol reader and prologue decoder on a longshot.exe FILE (the game is not run).
   Usage: selftest.exe <longshot.exe> ; prints each wanted symbol's RVA and whether its prologue is a known shape.
   Exit 0 = every symbol found and every lifted target hookable. */
#include "dropin.c"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    g_log = stdout;
    wchar_t exe[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, exe, MAX_PATH);
    /* the frame function and every guest function a 0005-layer wrap or an entry-converted hook would use */
    Want want[] = {{"f_83b6e300"}, {"hos_guest_base"}, {"hos_log"}, {"a64_lookup"}, {"hos_oot_menu_tick"},
                   {"hos_oot_free_view"}, {"hos_mouse_camera"}, {"f_80392a70"}, {"f_800c6490"}, {"f_804bd8c0"},
                   {"f_80606760"}, {"f_83426d70"}, {"f_834267d0"}, {"f_834267a0"}, {"f_82771790"}, {"f_82deee60"},
                   {"f_804d8240"}};
    int nwant = sizeof want / sizeof want[0];
    int all = find_symbols(exe, want, nwant);
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    read_at(f, 0, &dos, sizeof dos); read_at(f, dos.e_lfanew, &nt, sizeof nt);
    IMAGE_SECTION_HEADER sec[32];
    DWORD nsec = nt.FileHeader.NumberOfSections < 32 ? nt.FileHeader.NumberOfSections : 32;
    read_at(f, dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader, sec,
            nsec * sizeof sec[0]);
    int bad = 0;
    for (int w = 0; w < nwant; w++) {
        if (!want[w].found) { printf("MISSING %s\n", want[w].name); bad++; continue; }
        uint8_t b[16] = {0};
        for (DWORD s = 0; s < nsec; s++)
            if (want[w].rva >= sec[s].VirtualAddress && want[w].rva < sec[s].VirtualAddress + sec[s].Misc.VirtualSize)
                read_at(f, sec[s].PointerToRawData + (want[w].rva - sec[s].VirtualAddress), b, 16);
        int len = prologue_len(b);
        int lifted = want[w].name[0] == 'f' && want[w].name[1] == '_';
        printf("%-20s rva %#010x first %02x%02x%02x%02x%02x%02x%02x%02x  hook %s\n", want[w].name, want[w].rva,
               b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], len ? "OK" : "NO (unknown shape)");
        if (lifted && !len) bad++;
    }
    CloseHandle(f);
    printf("%s\n", all && !bad ? "SELFTEST PASS" : "SELFTEST FAIL");
    return all && !bad ? 0 : 1;
}

#pragma once
// ==========================================================================
// DbPassword — DB 암호를 읽는다. 암호는 커밋되는 INI 에 두지 않는다
//
//   커밋되는 INI : [DB] PasswordFile=<이름>.local.ini   상대 경로면 그 INI 옆
//   가리키는 파일 : [DB] Password=<값>                   git 이 무시한다
//                  새 PC 에서는 Run/bin/DbPassword.local.ini.example 을 보고 만든다
//
//   ★ 파일 이름을 코드에 고정하지 않는다 — 시험이 틀린 암호를 볼 때 진짜 파일을 덮지 않고
//     다른 파일을 가리키게 하려고. git 이 무시하는 파일은 대본이 도중에 죽으면 되돌릴 길이 없다.
//     운영 서버에서는 저장소 밖 경로를 가리키면 된다.
//   ★ INI 에 Password 가 남아 있으면 거절한다 — 조용히 무시하면 리포에 암호를 도로 적어도 모른다.
//   ★ 오류 문구에 값을 싣지 않는다 — 문구는 로그에 남는다.
// ==========================================================================

#include <string>

#include <Config/IniFile.h>

namespace DbPassword
{
    // ini = iniPath 에서 읽은 설정. 실패하면 error 에 무엇을 고칠지 담는다
    inline bool Read(const CIniFile& ini, const std::string& iniPath, std::string& password, std::string& error)
    {
        if (ini.HasKey("DB", "Password"))
        {
            error = "[DB] Password 는 INI 에 두지 않는다 — PasswordFile 이 가리키는 파일로 옮길 것: " + iniPath;
            return false;
        }

        const std::string file = ini.GetString("DB", "PasswordFile", "");
        if (file.empty())
        {
            error = "[DB] PasswordFile 이 없음: " + iniPath;
            return false;
        }

        const bool absolute = file[0] == '/' || file[0] == '\\' || (file.size() > 1 && file[1] == ':');
        const size_t slash = iniPath.find_last_of("/\\");
        const std::string path = (absolute || slash == std::string::npos) ? file : iniPath.substr(0, slash + 1) + file;

        CIniFile secret;
        if (!secret.Load(path))
        {
            error = "[DB] 암호 파일을 못 읽음: " + path + " — Run/bin/DbPassword.local.ini.example 을 보고 만들 것";
            return false;
        }
        const std::string value = secret.GetString("DB", "Password", "");
        if (value.empty())
        {
            error = "[DB] 암호 파일에 Password 가 없음: " + path;
            return false;
        }
        password = value;
        return true;
    }
}

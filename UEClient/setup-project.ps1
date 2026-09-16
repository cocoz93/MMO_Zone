<#
  setup-project.ps1 — 언리얼 클라 프로젝트를 엔진 템플릿에서 재구성한다.

  왜 스크립트인가:
    이 리포는 공개다. 엔진 템플릿 콘텐츠와 Fab 에셋(마네킹·Paragon)은 재배포할 수 없어
    Content/ 를 git 에서 제외했다. 대신 이 스크립트가 엔진 설치본에서 다시 만들어 준다.

  하는 일:
    1) Templates/TP_ThirdPerson 을 복사 (TemplateDefs.ini 의 제외 목록을 그대로 적용)
    2) TemplateResources/High 의 공유 콘텐츠 팩 3개를 Content/<이름>/ 으로 얹음
    3) 이름 치환 (폴더·파일명·파일 내용, 대문자/소문자/원형 3형태)
    4) .uproject 생성

  사용:
    .\setup-project.ps1                 # 처음 생성 (대상 폴더가 비어 있어야 한다)
    .\setup-project.ps1 -ContentOnly    # Source 는 git 에 있고 Content 만 복원할 때

  ※ 다시 만들려면 대상 폴더를 손으로 지운 뒤 실행할 것.
     스크립트가 폴더를 통째로 지우지는 않는다 (실수로 작업물을 날리지 않기 위해서다).
#>
param(
	[string]$EnginePath  = "C:\Program Files\Epic Games\UE_5.8",
	[string]$ProjectName = "MMOClient",
	[switch]$ContentOnly
)

$ErrorActionPreference = "Stop"

$TemplateName = "TP_ThirdPerson"
$Root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjDir = Join-Path $Root $ProjectName
$Tpl     = Join-Path $EnginePath "Templates\$TemplateName"
$Shared  = Join-Path $EnginePath "Templates\TemplateResources\High"

# 공유 콘텐츠 팩 — TemplateDefs.ini 의 SharedContentPacks 와 같아야 한다.
$Packs = @("LevelPrototyping", "Characters", "Input")

if (-not (Test-Path $Tpl))    { throw "템플릿을 찾을 수 없다: $Tpl  (-EnginePath 확인)" }
if (-not (Test-Path $Shared)) { throw "공유 콘텐츠를 찾을 수 없다: $Shared" }

Write-Host "엔진    : $EnginePath"
Write-Host "프로젝트: $ProjDir"

function Copy-SharedPacks {
	$ContentDir = Join-Path $ProjDir "Content"
	New-Item -ItemType Directory -Path $ContentDir -Force | Out-Null
	foreach ($pack in $Packs) {
		$src = Join-Path $Shared "$pack\Content"
		if (-not (Test-Path $src)) { Write-Warning "공유 팩 없음(건너뜀): $pack"; continue }
		Copy-Item $src (Join-Path $ContentDir $pack) -Recurse -Force
		Write-Host "  콘텐츠 팩: $pack"
	}
}

if ($ContentOnly) {
	if (-not (Test-Path $ProjDir)) { throw "프로젝트 폴더가 없다: $ProjDir  (-ContentOnly 없이 먼저 실행할 것)" }
	Copy-Item (Join-Path $Tpl "Content") (Join-Path $ProjDir "Content") -Recurse -Force
	Copy-SharedPacks
	Write-Host "`nContent 복원 완료." -ForegroundColor Green
	return
}

if (Test-Path (Join-Path $ProjDir "$ProjectName.uproject")) {
	throw "이미 프로젝트가 있다: $ProjDir`n다시 만들려면 이 폴더를 손으로 지운 뒤 실행할 것."
}
New-Item -ItemType Directory -Path $ProjDir -Force | Out-Null

# 1) 템플릿 복사 — TemplateDefs.ini 의 제외 목록
$SkipDirs  = @("Binaries","Build","Intermediate","Saved","Media")
$SkipRel   = @("Content\ThirdPerson\Animations","Content\ThirdPerson\Character")
$SkipFiles = @("$TemplateName.uproject","$TemplateName.png","TemplateDefs.ini","config.ini",
               "Manifest.json","contents.txt","$TemplateName.sln")

Get-ChildItem $Tpl -Recurse -File -Force | ForEach-Object {
	$rel = $_.FullName.Substring($Tpl.Length + 1)
	foreach ($d in $SkipDirs) { if ($rel -split '\\' -contains $d) { return } }
	foreach ($r in $SkipRel)  { if ($rel.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) { return } }
	if ($SkipFiles -contains $_.Name) { return }

	$dest = Join-Path $ProjDir $rel
	New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
	Copy-Item $_.FullName $dest -Force
}
Write-Host "  템플릿 복사 완료"

# 2) 공유 콘텐츠 팩
Copy-SharedPacks

# 3) 이름 치환 — TemplateDefs.ini 와 같은 순서: 대문자 -> 소문자 -> 원형(대소문자 무시)
$pairs = @(
	@{ From = $TemplateName.ToUpper(); To = $ProjectName.ToUpper(); Case = $true  },
	@{ From = $TemplateName.ToLower(); To = $ProjectName.ToLower(); Case = $true  },
	@{ From = $TemplateName;           To = $ProjectName;           Case = $false }
)
$TextExt = @(".cpp",".h",".ini",".cs")

# 3-a) 폴더명 (깊은 것부터)
Get-ChildItem $ProjDir -Recurse -Directory |
	Sort-Object { $_.FullName.Length } -Descending |
	Where-Object { $_.Name -like "*$TemplateName*" } |
	ForEach-Object { Rename-Item $_.FullName ($_.Name -replace [regex]::Escape($TemplateName), $ProjectName) -Force }

# 3-b) 파일 내용 (BOM 유무를 보존한다)
Get-ChildItem $ProjDir -Recurse -File | Where-Object { $TextExt -contains $_.Extension.ToLower() } | ForEach-Object {
	$bytes  = [IO.File]::ReadAllBytes($_.FullName)
	$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
	$text   = [IO.File]::ReadAllText($_.FullName)
	$orig   = $text
	foreach ($p in $pairs) {
		if ($p.Case) { $text = $text.Replace($p.From, $p.To) }
		else         { $text = [regex]::Replace($text, [regex]::Escape($p.From), $p.To, 'IgnoreCase') }
	}
	if ($text -ne $orig) {
		[IO.File]::WriteAllText($_.FullName, $text, (New-Object System.Text.UTF8Encoding($hasBom)))
	}
}

# 3-c) 파일명
Get-ChildItem $ProjDir -Recurse -File | Where-Object { $_.Name -like "*$TemplateName*" } | ForEach-Object {
	Rename-Item $_.FullName ($_.Name -replace [regex]::Escape($TemplateName), $ProjectName) -Force
}
Write-Host "  이름 치환 완료 ($TemplateName -> $ProjectName)"

# 4) .uproject — 템플릿 것은 제외 목록이라 여기서 새로 쓴다.
$uproject = [ordered]@{
	FileVersion       = 3
	EngineAssociation = "5.8"
	Category          = ""
	Description       = "MMOServer 에 붙는 언리얼 클라이언트 (리플리케이션 미사용, 자체 TCP)"
	Modules           = @([ordered]@{
		Name = $ProjectName; Type = "Runtime"; LoadingPhase = "Default"
		AdditionalDependencies = @("Engine","AIModule","UMG")
	})
	Plugins = @(
		[ordered]@{ Name = "ModelingToolsEditorMode"; Enabled = $true; TargetAllowList = @("Editor") },
		[ordered]@{ Name = "StateTree";               Enabled = $true },
		[ordered]@{ Name = "GameplayStateTree";       Enabled = $true }
	)
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText((Join-Path $ProjDir "$ProjectName.uproject"), $uproject,
	(New-Object System.Text.UTF8Encoding($false)))

# 5) CoreRedirects — .uasset 은 바이너리라 이름 치환이 닿지 않는다.
#    이게 없으면 BP_ThirdPersonGameMode 등이 옛 부모 클래스를 못 찾아 로드에 실패한다.
$IniPath = Join-Path $ProjDir "Config\DefaultEngine.ini"
$Ini = [IO.File]::ReadAllText($IniPath)
if ($Ini -notmatch "\[CoreRedirects\]") {
	$Redirects = @"

[CoreRedirects]
; 엔진 템플릿($TemplateName)에서 복사해 만든 프로젝트라, .uasset 안의 클래스 참조가 옛 이름으로 남아 있다.
+ClassRedirects=(OldName="/Script/$TemplateName.${TemplateName}GameMode",NewName="/Script/$ProjectName.${ProjectName}GameMode")
+ClassRedirects=(OldName="/Script/$TemplateName.${TemplateName}Character",NewName="/Script/$ProjectName.${ProjectName}Character")
+ClassRedirects=(OldName="/Script/$TemplateName.${TemplateName}PlayerController",NewName="/Script/$ProjectName.${ProjectName}PlayerController")
; 이름이 그대로인 나머지(Variant_* 등)는 패키지만 옮겨 주면 된다.
+PackageRedirects=(OldName="/Script/$TemplateName",NewName="/Script/$ProjectName")
"@
	[IO.File]::WriteAllText($IniPath, $Ini.TrimEnd() + "`r`n" + $Redirects + "`r`n",
		(New-Object System.Text.UTF8Encoding($false)))
	Write-Host "  CoreRedirects 추가"
}
Write-Host "`n생성 완료: $ProjDir" -ForegroundColor Green
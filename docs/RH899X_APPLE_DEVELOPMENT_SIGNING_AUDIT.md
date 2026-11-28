# RH-899X Apple Development 签名身份审计

环境：macOS 14.8.7（23J520），Xcode 16.2（16C5032a），Apple M1。

本任务只修复代码签名身份并审计 HAL 签名方案；**没有安装 HAL 驱动，
没有重启 CoreAudio，没有修改默认音频设备**。

---

## 1. 根因

`security find-identity -v -p codesigning` 初始只显示 `Barrot Local Code Signing`。
Apple Development 证书本身存在且参数正确，但 **login keychain 中没有与其
公钥匹配的私钥**。

具体证据：

- 证书 SKID / `hpky`：`8C1DC371C344E2233861BAE553A82A25CA28CC43`
- 初始三个可签名私钥的公钥哈希分别是
  `4540ECBB…`、`5DE51D3A…`、`459C8EAB…`
- 没有任何私钥的公钥哈希等于 `8C1DC371…`
- 因此 `find-identity` 无法组成有效身份。

另一个关键事实：**用户提供的 `Y9VP2N4968` 不是 Team ID。**

- 证书 Subject OU = `Q42GN4ADQP`
- Xcode Personal Team teamID = `Q42GN4ADQP`
- `codesign` 签名后的 `TeamIdentifier = Q42GN4ADQP`
- `Y9VP2N4968` 只是证书 CN 括号中的个人标识

构建脚本如果按 `Y9VP2N4968` 配 Team，会与 Xcode 实际 Team 不一致。

---

## 2. 证书与私钥现状（修复后）

| 项目 | 值 |
| --- | --- |
| Subject | `Apple Development: heshuangyin@gmail.com (Y9VP2N4968)` |
| Issuer | `Apple Worldwide Developer Relations Certification Authority` (G3) |
| SHA-1 | `C9A652FF85459725C1B5A02F6117C142AF46CFB3` |
| SHA-256 | `4C674E016BDA8F990B4C7BCDAFEE4DBE3B700F47218653814577D24385279948` |
| 有效期 | 2026-11-25 → 2027-11-25 |
| Extended Key Usage | Code Signing（critical） |
| Key Usage | Digital Signature（critical） |
| TeamIdentifier | `Q42GN4ADQP` |
| 信任链 | Apple WWDR G3 → Apple Root CA，`security verify-cert` 成功 |

私钥：

- 位于 `~/Library/Keychains/login.keychain-db`
- 公钥哈希 = `8C1DC371C344E2233861BAE553A82A25CA28CC43`，与证书 SKID 一致
- Keychain label 仍是历史遗留的
  `Apple Development: Alkaid He (Alkaid He)`，这是**标签不匹配**，不影响身份；
  `codesign` 已能用它成功签名。

---

## 3. 执行过的修复

1. 将命令行开发者目录从 Command Line Tools 切换到 Xcode：

   ```sh
   sudo xcode-select -s /Applications/Xcode.app/Contents/Developer
   ```

   之前 `xcodebuild` 不可用，因为 `xcode-select -p` 指向
   `/Library/Developer/CommandLineTools`。

2. 使用 Homebrew 安装本地工具 `xcodegen`（免费，仅生成 Xcode 工程）。

3. 新建最小 macOS App 工程 `cert-test/`：

   - `DEVELOPMENT_TEAM = Q42GN4ADQP`
   - `CODE_SIGN_STYLE = Automatic`
   - `CODE_SIGN_IDENTITY = Apple Development`
   - bundle id `com.barrot.certtest`

4. 执行：

   ```sh
   xcodebuild -project BarrotCertTest.xcodeproj \
     -target BarrotCertTest -configuration Debug \
     -allowProvisioningUpdates build
   ```

   Xcode 自动签名阶段输出：

   ```
   Signing Identity: "Apple Development: heshuangyin@gmail.com (Y9VP2N4968)"
   codesign --force --sign C9A652FF85459725C1B5A02F6117C142AF46CFB3 ...
   ```

   这次构建在 login keychain 中生成/关联了与证书匹配的私钥，
   `find-identity` 随即出现 Apple Development 身份。

5. 加固 HAL 脚本：

   - `barrot-hal/build.sh` 现在优先自动选择名字含
     `Apple Development` 的身份（不会误选 `Barrot Local Code Signing`），
     并加 `--timestamp=none`。
   - `barrot-hal/install.sh` 现在要求签名 Authority 为
     `Apple Development` 或 `Developer ID Application`，并执行
     `codesign --verify --strict --deep`；本地自签名也会被拒绝。

6. 用 Apple Development 重新签名 HAL bundle（**未安装**）：

   ```
   Authority=Apple Development: heshuangyin@gmail.com (Y9VP2N4968)
   Authority=Apple Worldwide Developer Relations Certification Authority
   Authority=Apple Root CA
   TeamIdentifier=Q42GN4ADQP
   ```

---

## 4. 修复前后 find-identity 对比

修复前：

```
1) 5E58D9… "Barrot Local Code Signing"
2) 5E58D9… "Barrot Local Code Signing"
   2 valid identities found
```

修复后：

```
1) 5E58D9… "Barrot Local Code Signing"
2) 5E58D9… "Barrot Local Code Signing"
3) C9A652… "Apple Development: heshuangyin@gmail.com (Y9VP2N4968)"
   3 valid identities found
```

仅 login keychain：

```
1) 5E58D9… "Barrot Local Code Signing"
2) C9A652… "Apple Development: heshuangyin@gmail.com (Y9VP2N4968)"
```

`Barrot Local Code Signing` 仍然存在，但 HAL 构建脚本不会再自动选择它。

---

## 5. 临时代码签名验证

临时文件放在 `cert-test/`，验证后已移出。

```sh
clang -o tmp-sign tmp-sign.c
codesign --force --sign C9A652FF85459725C1B5A02F6117C142AF46CFB3 tmp-sign
codesign --verify --strict --verbose=2 tmp-sign
```

结果：

```
tmp-sign: valid on disk
tmp-sign: satisfies its Designated Requirement
```

签名信息：

```
Authority=Apple Development: heshuangyin@gmail.com (Y9VP2N4968)
Authority=Apple Worldwide Developer Relations Certification Authority
Authority=Apple Root CA
TeamIdentifier=Q42GN4ADQP
```

临时二进制可正常执行（exit 0）。

同一个 Xcode 构建的 App 也可正常执行，entitlements 只有
`com.apple.security.get-task-allow`，没有 embedded provisioning profile。

---

## 6. 免费 Apple Development 对 AudioServerPlugIn 的可行性

已确认：

- 证书是 **Apple 签发**、Code Signing EKU、Apple Root 信任链完整。
- AMFI 之前拒绝 ad-hoc 的原因是 `No certificate chain found`；
  该原因现在不再成立。
- `Core-Audio-Driver-Service.helper` 具有
  `com.apple.security.cs.disable-library-validation = true`，
  不存在“必须同 Team ID”的 library validation 限制。
- HAL bundle 没有 entitlements；Apple Development 签名不需要额外 entitlement。
- HAL bundle `codesign --verify --strict --deep` 通过。
- `spctl -a -vv` 对 HAL bundle 返回：

  ```
  accepted
  origin=Apple Development: heshuangyin@gmail.com (Y9VP2N4968)
  ```

尚未确认：

- 系统级 HAL 加载尚未执行（本阶段禁止安装/重启 CoreAudio）。
- `amfid` 对 HAL 插件的策略是否还要求 embedded provisioning profile。
- Apple Development 签名是否会在 Core Audio Driver Service helper 内
  被 `-423` 或其他 AMFI 错误拒绝。

判断：

**身份级：可行。**
Apple Development 证书已经满足“Apple 签发 + Code Signing + 完整信任链”的
基本条件；`spctl` 和 `codesign` 都接受。

**系统 HAL 加载级：尚未证明。**
必须实际安装一次并查看 `amfid` 日志，才能确认 helper 不要求额外 profile。

---

## 7. 后续 HAL 加载验证所需的最小步骤

1. 构建并确认签名：

   ```sh
   cd /Users/alkaidhe/Documents/mac/cm108/barrot-hal
   ./build.sh
   codesign --verify --strict --deep build/RH899X.driver
   ```

2. 安装（下一步任务执行，本次未执行）：

   ```sh
   ./install.sh
   ```

3. 检查：

   ```sh
   security find-identity -v -p codesigning
   bridge/tests/list_audio
   sudo log show --last 2m --info --debug \
     --predicate 'process == "amfid" OR eventMessage CONTAINS[c] "RH899X"'
   ```

4. 若仍出现 `-423`：
   - 检查是否变成“provisioning profile”错误；
   - 为 bundle id `com.ranhe.rh899x.audio` 生成并嵌入开发 profile；
   - 若仍失败，再评估 Developer ID（付费，违反零成本约束）。

5. 枚举成功后，再做控制中心切换、IOProc 启停、64×24 USB 传输和物理听音。

---

## 8. 尚需用户完成的操作

签名身份本身不再需要用户操作。

后续系统 HAL 验证需要：

- 允许 `install.sh` 使用 sudo 安装并重启 `coreaudiod`；
- 一次物理听音验收。

本次任务按要求没有执行这些步骤。

## 9. 系统状态

- `/Library/Audio/Plug-Ins/HAL` 仍只有 BlackHole 与 ToDesk；
- 默认输出仍为 `MacBook Air扬声器`；
- 没有安装 `RH899X.driver`，没有重启 CoreAudio；
- `xcode-select` 现在指向 `/Applications/Xcode.app/Contents/Developer`；
- `xcodegen` 已安装（Homebrew，免费工具）；
- 临时签名文件已移到 `/tmp/barrot-sign-check/`。

//----------------------------------------------------------------------------------
//	png2apng - 連番PNG(RGBA)からAPNG(アルファ対応)を作る単独コマンドラインツール
//----------------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <algorithm>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "zlib/zlib.h"

//---------------------------------------------------------------------
//		PNGチャンク書き出しユーティリティ
//---------------------------------------------------------------------
static void put_be32(BYTE *p, unsigned int v)
{
	p[0] = (BYTE)(v >> 24);
	p[1] = (BYTE)(v >> 16);
	p[2] = (BYTE)(v >> 8);
	p[3] = (BYTE)v;
}
static void put_be16(BYTE *p, unsigned int v)
{
	p[0] = (BYTE)(v >> 8);
	p[1] = (BYTE)v;
}

static BOOL write_chunk(HANDLE fp, const char *type, const BYTE *data, DWORD len)
{
	BYTE	lenbuf[4], crcbuf[4];
	DWORD	dw;
	unsigned long crc;

	put_be32(lenbuf, len);
	if (!WriteFile(fp, lenbuf, 4, &dw, NULL)) return FALSE;
	if (!WriteFile(fp, type, 4, &dw, NULL)) return FALSE;
	if (len && !WriteFile(fp, data, len, &dw, NULL)) return FALSE;

	crc = crc32(0L, (const Bytef *)type, 4);
	if (len) crc = crc32(crc, data, len);
	put_be32(crcbuf, (unsigned int)crc);
	if (!WriteFile(fp, crcbuf, 4, &dw, NULL)) return FALSE;

	return TRUE;
}

//---------------------------------------------------------------------
//		PNGフィルタ (適応選択: 5種のフィルタから合計絶対値最小を選ぶ)
//---------------------------------------------------------------------
static int paeth_predictor(int a, int b, int c)
{
	int p = a + b - c;
	int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	if (pa <= pb && pa <= pc) return a;
	if (pb <= pc) return b;
	return c;
}

static void apply_filter(int type, const BYTE *cur, const BYTE *prev, int rowbytes, int bpp, std::vector<BYTE> &out)
{
	out.resize(rowbytes);
	for (int i = 0; i < rowbytes; i++) {
		int a = (i >= bpp) ? cur[i - bpp] : 0;
		int b = prev ? prev[i] : 0;
		int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
		int x = cur[i];
		int val;
		switch (type) {
			case 1:  val = x - a; break;
			case 2:  val = x - b; break;
			case 3:  val = x - ((a + b) >> 1); break;
			case 4:  val = x - paeth_predictor(a, b, c); break;
			default: val = x; break;
		}
		out[i] = (BYTE)val;
	}
}

static long sum_abs(const std::vector<BYTE> &buf)
{
	long s = 0;
	for (size_t i = 0; i < buf.size(); i++) {
		signed char v = (signed char)buf[i];
		s += (v < 0) ? -v : v;
	}
	return s;
}

static void choose_filter(const BYTE *cur, const BYTE *prev, int rowbytes, int bpp, BYTE *outWithType)
{
	std::vector<BYTE> cand, best;
	int bestType = 0;
	long bestSum = -1;

	for (int t = 0; t < 5; t++) {
		apply_filter(t, cur, prev, rowbytes, bpp, cand);
		long s = sum_abs(cand);
		if (bestSum < 0 || s < bestSum) {
			bestSum = s;
			bestType = t;
			best = cand;
		}
	}
	outWithType[0] = (BYTE)bestType;
	memcpy(outWithType + 1, best.data(), rowbytes);
}

//---------------------------------------------------------------------
//		連番PNGファイルの列挙 (フォルダ内 *.png を名前順に)
//---------------------------------------------------------------------
static bool list_png_files(const std::string &dir, std::vector<std::string> &out)
{
	WIN32_FIND_DATAA fd;
	std::string pattern = dir + "\\*.png";
	HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return false;
	do {
		if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			out.push_back(dir + "\\" + fd.cFileName);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end());
	return !out.empty();
}

//---------------------------------------------------------------------
//		AVI読み込み (無圧縮 BI_RGB 32bit のみ対応)
//---------------------------------------------------------------------
typedef struct {
	std::vector<BYTE> data;					//	ファイル全体
	std::vector<std::pair<size_t, DWORD>> frames;	//	各フレームの(データ開始オフセット,サイズ)
	int w, h;
	double fps;
} AVI_INFO;

//	4CCの先頭2文字がストリーム番号、後2文字が "db"(未圧縮画像) or "dc"(圧縮画像)
static bool is_video_chunk_id(const char *id)
{
	return isdigit((unsigned char)id[0]) && isdigit((unsigned char)id[1])
		&& (id[2] == 'd') && (id[3] == 'b' || id[3] == 'c');
}

static bool load_avi(const std::string &path, AVI_INFO &out)
{
	HANDLE fh = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE) return false;
	DWORD fsize = GetFileSize(fh, NULL);
	out.data.resize(fsize);
	DWORD readBytes = 0;
	BOOL ok = ReadFile(fh, out.data.data(), fsize, &readBytes, NULL);
	CloseHandle(fh);
	if (!ok || readBytes != fsize) return false;

	const BYTE *d = out.data.data();
	if (fsize < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "AVI ", 4) != 0) return false;

	out.w = out.h = 0;
	BOOL foundStrf = FALSE;
	double microSecPerFrame = 0;

	//	RIFFの直下(hdrl/movi等のLIST)を順に辿る
	size_t pos = 12;
	while (pos + 8 <= fsize) {
		char id[5] = { 0 }; memcpy(id, d + pos, 4);
		DWORD size = *(const DWORD *)(d + pos + 4);
		size_t bodyStart = pos + 8;

		if (!strcmp(id, "LIST") && bodyStart + 4 <= fsize) {
			char listType[5] = { 0 }; memcpy(listType, d + bodyStart, 4);
			size_t inner = bodyStart + 4, innerEnd = bodyStart + size;
			if (innerEnd > fsize) innerEnd = fsize;

			if (!strcmp(listType, "hdrl")) {
				//	avih / strl(strh,strf) を探索
				size_t p2 = inner;
				while (p2 + 8 <= innerEnd) {
					char id2[5] = { 0 }; memcpy(id2, d + p2, 4);
					DWORD size2 = *(const DWORD *)(d + p2 + 4);
					size_t body2 = p2 + 8;
					if (!strcmp(id2, "avih") && body2 + 4 <= innerEnd) {
						DWORD usPerFrame = *(const DWORD *)(d + body2);
						microSecPerFrame = (double)usPerFrame;
					} else if (!strcmp(id2, "LIST") && body2 + 4 <= innerEnd) {
						char lt2[5] = { 0 }; memcpy(lt2, d + body2, 4);
						if (!strcmp(lt2, "strl")) {
							size_t p3 = body2 + 4, end3 = body2 + size2;
							if (end3 > innerEnd) end3 = innerEnd;
							while (p3 + 8 <= end3) {
								char id3[5] = { 0 }; memcpy(id3, d + p3, 4);
								DWORD size3 = *(const DWORD *)(d + p3 + 4);
								size_t body3 = p3 + 8;
								if (!strcmp(id3, "strf") && body3 + 20 <= end3) {
									//	BITMAPINFOHEADER
									int biWidth = *(const int *)(d + body3 + 4);
									int biHeight = *(const int *)(d + body3 + 8);
									WORD biBitCount = *(const WORD *)(d + body3 + 14);
									DWORD biCompression = *(const DWORD *)(d + body3 + 16);
									if (biBitCount == 32 && biCompression == 0) {
										out.w = biWidth;
										out.h = (biHeight < 0) ? -biHeight : biHeight;
										foundStrf = TRUE;
									}
								}
								p3 += 8 + size3 + (size3 & 1);
							}
						}
					}
					p2 += 8 + size2 + (size2 & 1);
				}
			} else if (!strcmp(listType, "movi")) {
				size_t p2 = inner;
				while (p2 + 8 <= innerEnd) {
					char id2[5] = { 0 }; memcpy(id2, d + p2, 4);
					DWORD size2 = *(const DWORD *)(d + p2 + 4);
					size_t body2 = p2 + 8;
					if (is_video_chunk_id(id2)) {
						if (id2[3] == 'c') { return false; }	//	圧縮フレームは非対応
						out.frames.push_back(std::make_pair(body2, size2));
					}
					p2 += 8 + size2 + (size2 & 1);
				}
			}
		}
		pos += 8 + size + (size & 1);
	}

	if (!foundStrf || out.w <= 0 || out.h <= 0 || out.frames.empty()) return false;
	out.fps = (microSecPerFrame > 0.0) ? (1000000.0 / microSecPerFrame) : 30.0;
	return true;
}

//---------------------------------------------------------------------
//		使い方
//---------------------------------------------------------------------
static void print_usage()
{
	printf(
		"png2apng - 連番PNG(RGBA) または 無圧縮RGBA(32bit) AVI からAPNGを作成\n"
		"\n"
		"使い方:\n"
		"  png2apng <入力フォルダ or AVIファイル> <出力ファイル.png> [オプション]\n"
		"\n"
		"オプション:\n"
		"  --fps N     フレームレート (既定値: PNG=30 / AVI=ファイルのfps)\n"
		"  --loop N    繰り返し回数 (0=無限ループ, 既定値: 0)\n"
		"  --level N   zlib圧縮レベル 0-9 (既定値: 6)\n"
		"\n"
		"※AVIは AviUtl の「拡張編集 AVI/BMP出力(RGBA)」で\n"
		"  ビデオ圧縮=RGBA(32bit)非圧縮 で出力したものに対応。\n"
		"  UtVideo/Lagarith等の圧縮コーデックには非対応。\n"
	);
}

//---------------------------------------------------------------------
//		メイン
//---------------------------------------------------------------------
//	入力パスから出力ファイル名を自動生成する (ドラッグ&ドロップ用)
//	例: "C:\a\test1.avi" -> "C:\a\test1_apng.png" / "C:\a\frames" -> "C:\a\frames_apng.png"
static std::string make_default_output(const std::string &inputPath)
{
	std::string base = inputPath;
	while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
	size_t slash = base.find_last_of("\\/");
	std::string name = (slash == std::string::npos) ? base : base.substr(slash + 1);
	size_t dot = name.find_last_of('.');
	if (dot != std::string::npos) name = name.substr(0, dot);
	std::string dir = (slash == std::string::npos) ? "" : base.substr(0, slash + 1);
	return dir + name + "_apng.png";
}

static int run_main(int argc, char **argv, bool interactive)
{
	std::string inputPath = argv[1];
	std::string outputFile;
	int optStart;

	if (interactive) {
		//	ドラッグ&ドロップ: 出力ファイル名は自動生成
		outputFile = make_default_output(inputPath);
		optStart = 2;
	} else {
		outputFile = argv[2];
		optStart = 3;
	}

	double fps = 0.0;	//	0 = 未指定
	int loop = 0;
	int level = 6;

	for (int i = optStart; i < argc; i++) {
		if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
		else if (!strcmp(argv[i], "--loop") && i + 1 < argc) loop = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
		else { fprintf(stderr, "不明なオプション: %s\n", argv[i]); print_usage(); return 1; }
	}
	if (level < 0) level = 0;
	if (level > 9) level = 9;

	//	入力がフォルダ(連番PNG) か AVIファイル かを判定
	DWORD attr = GetFileAttributesA(inputPath.c_str());
	bool isDir = (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
	bool useAvi = false;
	std::vector<std::string> files;
	AVI_INFO avi;

	if (isDir) {
		if (!list_png_files(inputPath, files)) {
			fprintf(stderr, "エラー: 入力フォルダにPNGファイルが見つからない: %s\n", inputPath.c_str());
			return 1;
		}
		printf("%zu枚のPNGを検出\n", files.size());
		if (fps <= 0.0) fps = 30.0;
	} else {
		if (!load_avi(inputPath, avi)) {
			fprintf(stderr,
				"エラー: AVIを読み込めない、または非対応形式(無圧縮RGBA(32bit)のみ対応): %s\n",
				inputPath.c_str());
			return 1;
		}
		useAvi = true;
		printf("AVIから%zuフレーム検出 (%dx%d, %.3ffps)\n", avi.frames.size(), avi.w, avi.h, avi.fps);
		if (fps <= 0.0) fps = avi.fps;
	}

	size_t frameCount = useAvi ? avi.frames.size() : files.size();

	//	フレームレート -> delay_num/delay_den (既約分数, 16bit範囲)
	int out_rate = (int)(fps * 1000.0 + 0.5);
	int out_scale = 1000;
	{
		int a = out_rate, b = out_scale;
		while (b) { int t = a % b; a = b; b = t; }
		int g = a ? a : 1;
		out_rate /= g;
		out_scale /= g;
	}
	while (out_rate > 65535 || out_scale > 65535) {
		out_rate = (out_rate + 1) / 2;
		out_scale = (out_scale + 1) / 2;
		if (out_rate < 1) out_rate = 1;
		if (out_scale < 1) out_scale = 1;
	}

	int w = 0, h = 0;
	HANDLE fp = INVALID_HANDLE_VALUE;
	unsigned int seq = 0;
	int rowbytes = 0, bpp = 4;
	std::vector<BYTE> prevRow, curRow, filteredRow, imgFiltered;

	for (size_t i = 0; i < frameCount; i++) {
		unsigned char *img = NULL;		//	トップダウンRGBAのフレームデータへのポインタ
		std::vector<BYTE> aviFrameBuf;	//	AVIモード時の変換用バッファ
		int fw, fh;

		if (useAvi) {
			fw = avi.w; fh = avi.h;
			size_t off = avi.frames[i].first;
			DWORD len = avi.frames[i].second;
			if (len < (DWORD)(fw * fh * 4)) {
				fprintf(stderr, "エラー: AVIフレームサイズが不足 (frame %zu)\n", i);
				if (fp != INVALID_HANDLE_VALUE) CloseHandle(fp);
				return 1;
			}
			const BYTE *src = avi.data.data() + off;	//	BGRA, 下から上向き, パディング無し
			aviFrameBuf.resize((size_t)fw * fh * 4);
			for (int y = 0; y < fh; y++) {
				const BYTE *srow = src + (size_t)(fh - 1 - y) * fw * 4;
				BYTE *drow = aviFrameBuf.data() + (size_t)y * fw * 4;
				for (int x = 0; x < fw; x++) {
					drow[x * 4 + 0] = srow[x * 4 + 2];	//	R
					drow[x * 4 + 1] = srow[x * 4 + 1];	//	G
					drow[x * 4 + 2] = srow[x * 4 + 0];	//	B
					drow[x * 4 + 3] = srow[x * 4 + 3];	//	A
				}
			}
			img = aviFrameBuf.data();
		} else {
			int fc;
			img = stbi_load(files[i].c_str(), &fw, &fh, &fc, 4);	//	強制RGBA
			if (!img) {
				fprintf(stderr, "エラー: 画像を読み込めない: %s\n", files[i].c_str());
				if (fp != INVALID_HANDLE_VALUE) CloseHandle(fp);
				return 1;
			}
		}

		if (i == 0) {
			w = fw; h = fh;
			rowbytes = w * bpp;
			prevRow.assign(rowbytes, 0);
			curRow.resize(rowbytes);
			filteredRow.resize(rowbytes + 1);
			imgFiltered.reserve((size_t)(rowbytes + 1) * h);

			fp = CreateFileA(outputFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (fp == INVALID_HANDLE_VALUE) {
				fprintf(stderr, "エラー: 出力ファイルを作成できない: %s\n", outputFile.c_str());
				if (!useAvi) stbi_image_free(img);
				return 1;
			}

			DWORD dw;
			static const BYTE sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
			WriteFile(fp, sig, 8, &dw, NULL);

			BYTE ihdr[13];
			put_be32(ihdr + 0, (unsigned)w);
			put_be32(ihdr + 4, (unsigned)h);
			ihdr[8] = 8;	//	bit depth
			ihdr[9] = 6;	//	color type: truecolor + alpha (RGBA)
			ihdr[10] = 0;
			ihdr[11] = 0;
			ihdr[12] = 0;
			write_chunk(fp, "IHDR", ihdr, 13);

			BYTE actl[8];
			put_be32(actl + 0, (unsigned)frameCount);
			put_be32(actl + 4, (unsigned)(loop < 0 ? 0 : loop));
			write_chunk(fp, "acTL", actl, 8);
		} else if (fw != w || fh != h) {
			fprintf(stderr, "エラー: 画像サイズが一致しない (%dx%d)\n", fw, fh);
			if (!useAvi) stbi_image_free(img);
			CloseHandle(fp);
			return 1;
		}

		imgFiltered.clear();
		for (int y = 0; y < h; y++) {
			memcpy(curRow.data(), img + (size_t)y * w * bpp, rowbytes);
			choose_filter(curRow.data(), (y == 0) ? NULL : prevRow.data(), rowbytes, bpp, filteredRow.data());
			imgFiltered.insert(imgFiltered.end(), filteredRow.begin(), filteredRow.begin() + 1 + rowbytes);
			prevRow = curRow;
		}
		if (!useAvi) stbi_image_free(img);

		uLong srcLen = (uLong)imgFiltered.size();
		uLongf destLen = compressBound(srcLen);
		std::vector<BYTE> comp(destLen);
		int zret = compress2(comp.data(), &destLen, imgFiltered.data(), srcLen, level);
		if (zret != Z_OK) {
			fprintf(stderr, "エラー: 圧縮に失敗\n");
			CloseHandle(fp);
			return 1;
		}

		BYTE fctl[26];
		put_be32(fctl + 0, seq++);
		put_be32(fctl + 4, (unsigned)w);
		put_be32(fctl + 8, (unsigned)h);
		put_be32(fctl + 12, 0);
		put_be32(fctl + 16, 0);
		put_be16(fctl + 20, (unsigned)out_scale);
		put_be16(fctl + 22, (unsigned)out_rate);
		fctl[24] = 0;
		fctl[25] = 0;
		write_chunk(fp, "fcTL", fctl, 26);

		if (i == 0) {
			write_chunk(fp, "IDAT", comp.data(), (DWORD)destLen);
		} else {
			std::vector<BYTE> fdat(4 + destLen);
			put_be32(fdat.data(), seq++);
			memcpy(fdat.data() + 4, comp.data(), destLen);
			write_chunk(fp, "fdAT", fdat.data(), (DWORD)fdat.size());
		}

		printf("\r%zu/%zu フレーム処理中...", i + 1, frameCount);
		fflush(stdout);
	}
	printf("\n");

	write_chunk(fp, "IEND", NULL, 0);
	CloseHandle(fp);

	printf("完成: %s\n", outputFile.c_str());
	return 0;
}

//---------------------------------------------------------------------
//		エントリポイント
//		exeにファイル/フォルダをドラッグ&ドロップした場合(引数1個)は
//		出力先を自動生成し、終了時にキー入力待ちでウィンドウを閉じない。
//---------------------------------------------------------------------
int main(int argc, char **argv)
{
	if (argc < 2) {
		print_usage();
		system("pause");
		return 1;
	}

	bool interactive = (argc == 2) || (argc >= 3 && argv[2][0] == '-');
	int ret = run_main(argc, argv, interactive);

	if (interactive) {
		printf("\n");
		system("pause");
	}
	return ret;
}

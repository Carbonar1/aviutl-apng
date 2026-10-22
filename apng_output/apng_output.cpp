//----------------------------------------------------------------------------------
//	APNG出力プラグイン for AviUtl (無印, 出力プラグインSDK output.h 準拠)
//----------------------------------------------------------------------------------
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "output.h"
#include "apng_output.h"
#include "zlib/zlib.h"

//---------------------------------------------------------------------
//		設定データ
//---------------------------------------------------------------------
typedef struct {
	double	fps;	//	出力フレームレート (0以下 = プロジェクトの設定を使う)
	int		loop;	//	繰り返し回数 (0 = 無限ループ)
	int		level;	//	zlib圧縮レベル (0-9)
} CONFIG;

static CONFIG config = { 0.0, 0, 6 };

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

//	1行分をフィルタし、outWithType[0]=フィルタ種別 outWithType[1..rowbytes]=フィルタ後データ に書き込む
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
//		出力プラグイン出力関数
//---------------------------------------------------------------------
BOOL func_output(OUTPUT_INFO *oip)
{
	if (!(oip->flag & OUTPUT_INFO_FLAG_VIDEO)) return FALSE;

	int w = oip->w, h = oip->h;
	int src_rate = oip->rate, src_scale = oip->scale;
	int src_n = oip->n;
	if (w <= 0 || h <= 0 || src_n <= 0 || src_rate <= 0 || src_scale <= 0) return FALSE;

	//	出力フレームレートの決定
	int out_rate, out_scale;
	if (config.fps > 0.0) {
		out_scale = 1000;
		out_rate = (int)(config.fps * 1000.0 + 0.5);
		if (out_rate < 1) out_rate = 1;
	} else {
		out_rate = src_rate;
		out_scale = src_scale;
	}
	//	既約分数化
	{
		int a = out_rate, b = out_scale;
		while (b) { int t = a % b; a = b; b = t; }
		int g = a ? a : 1;
		out_rate /= g;
		out_scale /= g;
	}
	//	fcTLのdelay_num/delay_denは16bitなので収まるようにする
	while (out_rate > 65535 || out_scale > 65535) {
		out_rate = (out_rate + 1) / 2;
		out_scale = (out_scale + 1) / 2;
		if (out_rate < 1) out_rate = 1;
		if (out_scale < 1) out_scale = 1;
	}

	//	出力フレーム数と、各出力フレームに対応する元フレーム番号を決定
	double src_duration = (double)src_n * (double)src_scale / (double)src_rate;
	int out_n = (int)(src_duration * out_rate / out_scale + 0.5);
	if (out_n < 1) out_n = 1;

	std::vector<int> frame_index(out_n);
	for (int i = 0; i < out_n; i++) {
		double t = (double)i * out_scale / out_rate;
		int sf = (int)(t * src_rate / src_scale + 0.5);
		if (sf >= src_n) sf = src_n - 1;
		if (sf < 0) sf = 0;
		frame_index[i] = sf;
	}

	int stride = ((w * 3 + 3) / 4) * 4;	//	DIB(BGR24bit)の1行バイト数(4バイト境界)
	int rowbytes = w * 3;					//	PNG側の1行バイト数(パディング無し)

	HANDLE fp = CreateFile(oip->savefile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fp == INVALID_HANDLE_VALUE) return FALSE;

	DWORD dw;
	static const BYTE sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	WriteFile(fp, sig, 8, &dw, NULL);

	BYTE ihdr[13];
	put_be32(ihdr + 0, (unsigned)w);
	put_be32(ihdr + 4, (unsigned)h);
	ihdr[8] = 8;	//	bit depth
	ihdr[9] = 2;	//	color type: truecolor(RGB)
	ihdr[10] = 0;	//	compression method
	ihdr[11] = 0;	//	filter method
	ihdr[12] = 0;	//	interlace method
	write_chunk(fp, "IHDR", ihdr, 13);

	BYTE actl[8];
	put_be32(actl + 0, (unsigned)out_n);
	put_be32(actl + 4, (unsigned)(config.loop < 0 ? 0 : config.loop));
	write_chunk(fp, "acTL", actl, 8);

	unsigned int seq = 0;
	std::vector<BYTE> prevRow(rowbytes, 0), curRow(rowbytes);
	std::vector<BYTE> filteredRow(rowbytes + 1);
	std::vector<BYTE> imgFiltered;
	imgFiltered.reserve((size_t)(rowbytes + 1) * h);

	BOOL result = TRUE;

	for (int i = 0; i < out_n; i++) {
		if (oip->func_is_abort()) { result = FALSE; break; }
		oip->func_rest_time_disp(i, out_n);

		int sf = frame_index[i];
		BYTE *pixelp = (BYTE *)oip->func_get_video_ex(sf, 0);	//	BGR24bit, 下から上向きDIB
		if (!pixelp) { result = FALSE; break; }

		imgFiltered.clear();

		for (int y = 0; y < h; y++) {
			BYTE *dibRow = pixelp + (size_t)(h - 1 - y) * stride;
			for (int x = 0; x < w; x++) {
				curRow[x * 3 + 0] = dibRow[x * 3 + 2];	//	R
				curRow[x * 3 + 1] = dibRow[x * 3 + 1];	//	G
				curRow[x * 3 + 2] = dibRow[x * 3 + 0];	//	B
			}
			choose_filter(curRow.data(), (y == 0) ? NULL : prevRow.data(), rowbytes, 3, filteredRow.data());
			imgFiltered.insert(imgFiltered.end(), filteredRow.begin(), filteredRow.begin() + 1 + rowbytes);
			prevRow = curRow;
		}

		uLong srcLen = (uLong)imgFiltered.size();
		uLongf destLen = compressBound(srcLen);
		std::vector<BYTE> comp(destLen);
		int zret = compress2(comp.data(), &destLen, imgFiltered.data(), srcLen, config.level);
		if (zret != Z_OK) { result = FALSE; break; }

		BYTE fctl[26];
		put_be32(fctl + 0, seq++);
		put_be32(fctl + 4, (unsigned)w);
		put_be32(fctl + 8, (unsigned)h);
		put_be32(fctl + 12, 0);
		put_be32(fctl + 16, 0);
		put_be16(fctl + 20, (unsigned)out_scale);	//	delay_num
		put_be16(fctl + 22, (unsigned)out_rate);	//	delay_den
		fctl[24] = 0;	//	dispose_op: none
		fctl[25] = 0;	//	blend_op: source
		write_chunk(fp, "fcTL", fctl, 26);

		if (i == 0) {
			write_chunk(fp, "IDAT", comp.data(), (DWORD)destLen);
		} else {
			std::vector<BYTE> fdat(4 + destLen);
			put_be32(fdat.data(), seq++);
			memcpy(fdat.data() + 4, comp.data(), destLen);
			write_chunk(fp, "fdAT", fdat.data(), (DWORD)fdat.size());
		}

		oip->func_update_preview();
	}

	if (result) write_chunk(fp, "IEND", NULL, 0);
	CloseHandle(fp);
	if (!result) DeleteFile(oip->savefile);

	return result;
}

//---------------------------------------------------------------------
//		出力プラグイン設定ダイアログ
//---------------------------------------------------------------------
static LRESULT CALLBACK func_config_proc(HWND hdlg, UINT umsg, WPARAM wparam, LPARAM lparam)
{
	switch (umsg) {
		case WM_INITDIALOG:
		{
			char buf[64];
			if (config.fps > 0.0) sprintf(buf, "%.3f", config.fps);
			else buf[0] = '\0';
			SetDlgItemText(hdlg, IDC_EDIT_FPS, buf);
			SetDlgItemInt(hdlg, IDC_EDIT_LOOP, (UINT)config.loop, FALSE);
			SetDlgItemInt(hdlg, IDC_EDIT_LEVEL, (UINT)config.level, FALSE);
			return TRUE;
		}
		case WM_COMMAND:
			switch (LOWORD(wparam)) {
				case IDOK:
				{
					char fpsbuf[64];
					GetDlgItemText(hdlg, IDC_EDIT_FPS, fpsbuf, sizeof(fpsbuf));
					double v = atof(fpsbuf);
					config.fps = (v > 0.0) ? v : 0.0;

					BOOL ok;
					int loop = (int)GetDlgItemInt(hdlg, IDC_EDIT_LOOP, &ok, FALSE);
					config.loop = (loop < 0) ? 0 : loop;

					int level = (int)GetDlgItemInt(hdlg, IDC_EDIT_LEVEL, &ok, FALSE);
					if (level < 0) level = 0;
					if (level > 9) level = 9;
					config.level = level;

					EndDialog(hdlg, LOWORD(wparam));
					break;
				}
				case IDCANCEL:
					EndDialog(hdlg, LOWORD(wparam));
					break;
			}
			break;
	}
	return FALSE;
}

BOOL func_config(HWND hwnd, HINSTANCE dll_hinst)
{
	DialogBox(dll_hinst, "CONFIG", hwnd, (DLGPROC)func_config_proc);
	return TRUE;
}

int func_config_get(void *data, int size)
{
	if (data) memcpy(data, &config, sizeof(config));
	return sizeof(config);
}

int func_config_set(void *data, int size)
{
	if (size != sizeof(config)) return 0;
	memcpy(&config, data, size);
	return size;
}

//---------------------------------------------------------------------
//		出力プラグイン構造体定義
//---------------------------------------------------------------------
static OUTPUT_PLUGIN_TABLE output_plugin_table = {
	NULL,											//	フラグ
	(LPSTR)"APNG出力2",									//	プラグインの名前
	(LPSTR)"APNGファイル (*.png)\0*.png\0すべてのファイル (*.*)\0*.*\0",	//	ファイルのフィルタ
	(LPSTR)"APNG出力プラグイン version 0.01 (自作)",		//	プラグインの情報
	NULL,											//	func_init
	NULL,											//	func_exit
	func_output,									//	func_output
	func_config,									//	func_config
	func_config_get,								//	func_config_get
	func_config_set,								//	func_config_set
};

//---------------------------------------------------------------------
//		出力プラグイン構造体のポインタを渡す関数
//---------------------------------------------------------------------
EXTERN_C OUTPUT_PLUGIN_TABLE __declspec(dllexport) * __stdcall GetOutputPluginTable(void)
{
	return &output_plugin_table;
}

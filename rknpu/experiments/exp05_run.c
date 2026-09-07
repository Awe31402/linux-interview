/*
 * exp05_run.c — 實驗 5.2 的載具
 *
 * 一個最小的 .rknn 執行器：載入模型、餵一份全 0 的輸入、跑一次、結束。
 * 目的不是得到正確答案，而是「讓 NPU 動起來」，好讓 rkspy.c 攔下 regcmd。
 *
 * 這樣就能拿任何模型做對照，不受範例程式綁定的圖片尺寸限制。
 *
 *   gcc -O1 -o exp05_run exp05_run.c -lrknnrt
 *   sudo env LD_PRELOAD=./rkspy.so RKSPY_LOG=/tmp/x.log ./exp05_run model.rknn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rknn_api.h>

static void *slurp(const char *path, uint32_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); return NULL; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *p = malloc(n);
	if (fread(p, 1, n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
	fclose(f); *len = (uint32_t)n; return p;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"用法: %s <model.rknn> [core_mask] [次數]\n"
			"  core_mask: 0=AUTO(預設) 1=core0 2=core1 4=core2 3=core0+1 7=三核\n",
			argv[0]);
		return 1;
	}
	int want_mask = argc > 2 ? atoi(argv[2]) : -1;
	int loops     = argc > 3 ? atoi(argv[3]) : 1;

	uint32_t mlen = 0;
	void *model = slurp(argv[1], &mlen);
	if (!model) return 1;

	rknn_context ctx = 0;
	int ret = rknn_init(&ctx, model, mlen, 0, NULL);
	free(model);
	if (ret < 0) { fprintf(stderr, "rknn_init 失敗: %d\n", ret); return 1; }

	if (want_mask >= 0) {
		ret = rknn_set_core_mask(ctx, (rknn_core_mask)want_mask);
		printf("rknn_set_core_mask(%d) -> %d\n", want_mask, ret);
	}

	rknn_input_output_num ion;
	rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &ion, sizeof(ion));
	printf("模型: %s\n輸入 %u 個、輸出 %u 個\n", argv[1], ion.n_input, ion.n_output);

	rknn_input *inputs = calloc(ion.n_input, sizeof(*inputs));
	void **bufs = calloc(ion.n_input, sizeof(*bufs));

	for (uint32_t i = 0; i < ion.n_input; i++) {
		rknn_tensor_attr a; memset(&a, 0, sizeof(a)); a.index = i;
		rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a));
		printf("  input[%u] dims=[", i);
		for (uint32_t d = 0; d < a.n_dims; d++)
			printf("%u%s", a.dims[d], d + 1 < a.n_dims ? ", " : "");
		printf("]  size=%u  fmt=%s  type=%s\n", a.size,
		       get_format_string(a.fmt), get_type_string(a.type));

		bufs[i] = calloc(1, a.size);      /* 全 0 輸入就夠了 */
		inputs[i].index = i;
		inputs[i].buf = bufs[i];
		inputs[i].size = a.size;
		inputs[i].pass_through = 0;       /* 讓 runtime 依下面的型別/排列自行處理 */
		inputs[i].type = a.type;
		inputs[i].fmt = a.fmt;
	}

	ret = rknn_inputs_set(ctx, ion.n_input, inputs);
	if (ret < 0) { fprintf(stderr, "rknn_inputs_set 失敗: %d\n", ret); return 1; }

	for (int n = 0; n < loops; n++) {
		ret = rknn_run(ctx, NULL);
		if (ret < 0) { fprintf(stderr, "rknn_run 失敗: %d\n", ret); break; }
	}
	printf("rknn_run x%d -> %d\n", loops, ret);

	for (uint32_t i = 0; i < ion.n_input; i++) free(bufs[i]);
	free(bufs); free(inputs);
	rknn_destroy(ctx);
	return 0;
}

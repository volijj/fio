/*
 * IO verification helpers
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

#include "fio.h"

static void fill_random_bytes(struct thread_data *td,
			      unsigned char *p, unsigned int len)
{
	unsigned int todo;
	int r;

	while (len) {
		r = os_random_long(&td->verify_state);

		/*
		 * lrand48_r seems to be broken and only fill the bottom
		 * 32-bits, even on 64-bit archs with 64-bit longs
		 */
		todo = sizeof(r);
		if (todo > len)
			todo = len;

		memcpy(p, &r, todo);

		len -= todo;
		p += todo;
	}
}

static void hexdump(void *buffer, int len)
{
	unsigned char *p = buffer;
	int i;

	for (i = 0; i < len; i++)
		log_info("%02x", p[i]);
	log_info("\n");
}

static int verify_io_u_crc7(struct verify_header *hdr, struct io_u *io_u)
{
	unsigned char *p = io_u->buf;
	unsigned char c;

	p += sizeof(*hdr);
	c = crc7(p, hdr->len - sizeof(*hdr));

	if (c != hdr->crc7) {
		log_err("crc7: verify failed at %llu/%lu\n", io_u->offset, io_u->buflen);
		log_err("crc7: wanted %x, got %x\n", hdr->crc7, c);
		return 1;
	}

	return 0;
}

static int verify_io_u_crc16(struct verify_header *hdr, struct io_u *io_u)
{
	unsigned char *p = io_u->buf;
	unsigned short c;

	p += sizeof(*hdr);
	c = crc16(p, hdr->len - sizeof(*hdr));

	if (c != hdr->crc16) {
		log_err("crc16: verify failed at %llu/%lu\n", io_u->offset, io_u->buflen);
		log_err("crc16: wanted %x, got %x\n", hdr->crc16, c);
		return 1;
	}

	return 0;
}

static int verify_io_u_crc64(struct verify_header *hdr, struct io_u *io_u)
{
	unsigned char *p = io_u->buf + sizeof(*hdr);
	unsigned long long c;

	c = crc64(p, hdr->len - sizeof(*hdr));

	if (c != hdr->crc64) {
		log_err("crc64: verify failed at %llu/%lu\n", io_u->offset, io_u->buflen);
		log_err("crc64: wanted %llx, got %llx\n", hdr->crc64, c);
		return 1;
	}

	return 0;
}

static int verify_io_u_crc32(struct verify_header *hdr, struct io_u *io_u)
{
	unsigned char *p = io_u->buf;
	unsigned long c;

	p += sizeof(*hdr);
	c = crc32(p, hdr->len - sizeof(*hdr));

	if (c != hdr->crc32) {
		log_err("crc32: verify failed at %llu/%lu\n", io_u->offset, io_u->buflen);
		log_err("crc32: wanted %lx, got %lx\n", hdr->crc32, c);
		return 1;
	}

	return 0;
}

static int verify_io_u_md5(struct verify_header *hdr, struct io_u *io_u)
{
	unsigned char *p = io_u->buf + sizeof(*hdr);
	uint32_t hash[MD5_HASH_WORDS];
	struct md5_ctx md5_ctx = {
		.hash = hash,
	};

	md5_update(&md5_ctx, p, hdr->len - sizeof(*hdr));

	if (memcmp(hdr->md5_digest, md5_ctx.hash, sizeof(hash))) {
		log_err("md5: verify failed at %llu/%lu\n", io_u->offset, io_u->buflen);
		hexdump(hdr->md5_digest, sizeof(hdr->md5_digest));
		hexdump(md5_ctx.hash, sizeof(hash));
		return 1;
	}

	return 0;
}

int verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	struct verify_header *hdr = (struct verify_header *) io_u->buf;
	int ret;

	if (td->o.verify == VERIFY_NULL || io_u->ddir != DDIR_READ)
		return 0;

	if (hdr->fio_magic != FIO_HDR_MAGIC) {
		log_err("Bad verify header %x\n", hdr->fio_magic);
		return EIO;
	}

	switch (hdr->verify_type) {
	case VERIFY_MD5:
		ret = verify_io_u_md5(hdr, io_u);
		break;
	case VERIFY_CRC64:
		ret = verify_io_u_crc64(hdr, io_u);
		break;
	case VERIFY_CRC32:
		ret = verify_io_u_crc32(hdr, io_u);
		break;
	case VERIFY_CRC16:
		ret = verify_io_u_crc16(hdr, io_u);
		break;
	case VERIFY_CRC7:
		ret = verify_io_u_crc7(hdr, io_u);
		break;
	default:
		log_err("Bad verify type %u\n", hdr->verify_type);
		ret = 1;
	}

	if (ret)
		return EIO;

	return 0;
}

static void fill_crc7(struct verify_header *hdr, void *p, unsigned int len)
{
	hdr->crc7 = crc7(p, len);
}

static void fill_crc16(struct verify_header *hdr, void *p, unsigned int len)
{
	hdr->crc16 = crc16(p, len);
}

static void fill_crc32(struct verify_header *hdr, void *p, unsigned int len)
{
	hdr->crc32 = crc32(p, len);
}

static void fill_crc64(struct verify_header *hdr, void *p, unsigned int len)
{
	hdr->crc64 = crc64(p, len);
}

static void fill_md5(struct verify_header *hdr, void *p, unsigned int len)
{
	struct md5_ctx md5_ctx = {
		.hash = (uint32_t *) hdr->md5_digest,
	};

	md5_update(&md5_ctx, p, len);
}

/*
 * fill body of io_u->buf with random data and add a header with the
 * crc32 or md5 sum of that data.
 */
void populate_verify_io_u(struct thread_data *td, struct io_u *io_u)
{
	const unsigned int len = io_u->buflen - sizeof(struct verify_header);
	struct verify_header *hdr;
	unsigned char *p;

	if (td->o.verify == VERIFY_NULL)
		return;

	hdr = (struct verify_header *) io_u->buf;
	hdr->fio_magic = FIO_HDR_MAGIC;
	hdr->len = io_u->buflen;
	hdr->verify_type = td->o.verify;

	p = io_u->buf + sizeof(*hdr);
	fill_random_bytes(td, p, len);

	switch (td->o.verify) {
	case VERIFY_MD5:
		fill_md5(hdr, p, len);
		break;
	case VERIFY_CRC64:
		fill_crc64(hdr, p, len);
		break;
	case VERIFY_CRC32:
		fill_crc32(hdr, p, len);
		break;
	case VERIFY_CRC16:
		fill_crc16(hdr, p, len);
		break;
	case VERIFY_CRC7:
		fill_crc7(hdr, p, len);
		break;
	default:
		log_err("fio: bad verify type: %d\n", td->o.verify);
		assert(0);
	}
}

int get_next_verify(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = NULL;

	/*
	 * this io_u is from a requeue, we already filled the offsets
	 */
	if (io_u->file)
		return 0;

	if (!RB_EMPTY_ROOT(&td->io_hist_tree)) {
		struct rb_node *n = rb_first(&td->io_hist_tree);

		ipo = rb_entry(n, struct io_piece, rb_node);
		rb_erase(n, &td->io_hist_tree);
	} else if (!list_empty(&td->io_hist_list)) {
		ipo = list_entry(td->io_hist_list.next, struct io_piece, list);
		list_del(&ipo->list);
	}

	if (ipo) {
		io_u->offset = ipo->offset;
		io_u->buflen = ipo->len;
		io_u->file = ipo->file;

		if ((io_u->file->flags & FIO_FILE_OPEN) == 0) {
			int r = td_io_open_file(td, io_u->file);

			if (r)
				return 1;
		}

		get_file(ipo->file);
		assert(io_u->file->flags & FIO_FILE_OPEN);
		io_u->ddir = DDIR_READ;
		io_u->xfer_buf = io_u->buf;
		io_u->xfer_buflen = io_u->buflen;
		free(ipo);
		return 0;
	}

	return 1;
}

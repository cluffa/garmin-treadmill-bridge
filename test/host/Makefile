CORE = ../../components/bridge_core
NRF  = ../../boards/xiao-nrf52840
CFLAGS = -I$(CORE) -I$(NRF) -Wall -Wextra -std=c11
TESTS = test_ftms_parse test_rsc_encode test_ftms_devlist test_ifit_parse test_ctrl_dispatch test_ant_sdm_encode test_ifit_poll test_connect_policy test_ctrl_frames

all: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

test_ftms_parse: test_ftms_parse.c $(CORE)/ftms_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_rsc_encode: test_rsc_encode.c $(CORE)/rsc_encode.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ftms_devlist: test_ftms_devlist.c $(CORE)/ftms_devlist.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ifit_parse: test_ifit_parse.c $(CORE)/ifit_parse.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ctrl_dispatch: test_ctrl_dispatch.c $(CORE)/ctrl_dispatch.c $(CORE)/ftms_devlist.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ant_sdm_encode: test_ant_sdm_encode.c $(CORE)/ant_sdm_encode.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ifit_poll: test_ifit_poll.c $(NRF)/ifit_poll.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_connect_policy: test_connect_policy.c $(CORE)/connect_policy.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ctrl_frames: test_ctrl_frames.c $(CORE)/ctrl_frames.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f $(TESTS)

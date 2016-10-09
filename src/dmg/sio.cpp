// GB Enhanced Copyright Daniel Baxter 2016
// Licensed under the GPLv2
// See LICENSE.txt for full license text

// File : sio.cpp
// Date : April 30, 2016
// Description : Game Boy Serial Input-Output emulation
//
// Sets up SDL networking
// Emulates Gameboy-to-Gameboy data transfers

#include <ctime>

#include "sio.h"
#include "common/util.h"

/****** SIO Constructor ******/
DMG_SIO::DMG_SIO()
{
	network_init = false;

	reset();
}

/****** SIO Destructor ******/
DMG_SIO::~DMG_SIO()
{
	#ifdef GBE_NETPLAY

	//Close SDL_net and any current connections
	if(server.host_socket != NULL)
	{
		SDLNet_TCP_DelSocket(tcp_sockets, server.host_socket);
		SDLNet_TCP_Close(server.host_socket);
	}

	if(server.remote_socket != NULL)
	{
		SDLNet_TCP_DelSocket(tcp_sockets, server.remote_socket);
		SDLNet_TCP_Close(server.remote_socket);
	}

	if(sender.host_socket != NULL)
	{
		//Send disconnect byte to another system first
		u8 temp_buffer[2];
		temp_buffer[0] = 0;
		temp_buffer[1] = 0x80;
		
		SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2);

		SDLNet_TCP_DelSocket(tcp_sockets, sender.host_socket);
		SDLNet_TCP_Close(sender.host_socket);
	}

	SDLNet_Quit();

	network_init = false;

	#endif

	std::cout<<"SIO::Shutdown\n";
}

/****** Initialize SIO ******/
bool DMG_SIO::init()
{
	#ifdef GBE_NETPLAY

	//Do not set up SDL_net if netplay is not enabled
	if(!config::use_netplay)
	{
		std::cout<<"SIO::Initialized\n";
		return false;
	}

	//Setup SDL_net
	if(SDLNet_Init() < 0)
	{
		std::cout<<"SIO::Error - Could not initialize SDL_net\n";
		return false;
	}

	network_init = true;

	//Server info
	server.host_socket = NULL;
	server.remote_socket = NULL;
	server.connected = false;
	server.port = config::netplay_server_port;

	//Client info
	sender.host_socket = NULL;
	sender.connected = false;
	sender.port = config::netplay_client_port;

	//Setup server, resolve the server with NULL as the hostname, the server will now listen for connections
	if(SDLNet_ResolveHost(&server.host_ip, NULL, server.port) < 0)
	{
		std::cout<<"SIO::Error - Server could not resolve hostname\n";
		return -1;
	}

	//Open a connection to listen on host's port
	if(!(server.host_socket = SDLNet_TCP_Open(&server.host_ip)))
	{
		std::cout<<"SIO::Error - Server could not open a connection on Port " << server.port << "\n";
		return -1;
	}

	//Setup client, listen on another port
	if(SDLNet_ResolveHost(&sender.host_ip, config::netplay_client_ip.c_str(), sender.port) < 0)
	{
		std::cout<<"SIO::Error - Client could not resolve hostname\n";
		return -1;
	}

	//Create sockets sets
	tcp_sockets = SDLNet_AllocSocketSet(3);

	//Initialize hard syncing
	if(config::netplay_hard_sync)
	{
		//The instance with the highest server port will start off waiting in sync mode
		sio_stat.sync_counter = (config::netplay_server_port > config::netplay_client_port) ? 64 : 0;
	}

	#endif

	std::cout<<"SIO::Initialized\n";
	return true;
}

/****** Reset SIO ******/
void DMG_SIO::reset()
{
	//General SIO
	sio_stat.connected = false;
	sio_stat.active_transfer = false;
	sio_stat.double_speed = false;
	sio_stat.internal_clock = false;
	sio_stat.shifts_left = 0;
	sio_stat.shift_counter = 0;
	sio_stat.shift_clock = 512;
	sio_stat.sync_counter = 0;
	sio_stat.sync_clock = 32;
	sio_stat.sync = false;
	sio_stat.transfer_byte = 0;
	sio_stat.sio_type = config::use_gb_printer ? GB_PRINTER : NO_GB_DEVICE;

	//GB Printer
	printer.scanline_buffer.clear();
	printer.scanline_buffer.resize(0x5A00, 0x0);
	printer.packet_buffer.clear();
	printer.packet_size = 0;
	printer.current_state = GBP_AWAITING_PACKET;
	printer.pal[0] = printer.pal[1] = printer.pal[2] = printer.pal[3] = 0;

	printer.command = 0;
	printer.compression_flag = 0;
	printer.strip_count = 0;
	printer.data_length = 0;
	printer.checksum = 0;
	printer.status = 0;

	//GB Mobile Adapter
	mobile_adapter.data.clear();
	mobile_adapter.data.resize(0xC0, 0x0);
	mobile_adapter.packet_buffer.clear();
	mobile_adapter.packet_size = 0;
	mobile_adapter.current_state = GBMA_AWAITING_PACKET;

	mobile_adapter.command = 0;
	mobile_adapter.data_length = 0;
	mobile_adapter.checksum = 0;

	#ifdef GBE_NETPLAY

	//Close any current connections
	if(network_init)
	{
		if(server.host_socket != NULL)
		{
			SDLNet_TCP_Close(server.host_socket);
		}

		if(server.remote_socket != NULL)
		{
			SDLNet_TCP_Close(server.remote_socket);
		}

		if(sender.host_socket != NULL)
		{
			//Send disconnect byte to another system first
			u8 temp_buffer[2];
			temp_buffer[0] = 0;
			temp_buffer[1] = 0x80;
		
			SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2);

			SDLNet_TCP_Close(sender.host_socket);
		}
	}

	//Server info
	server.host_socket = NULL;
	server.remote_socket = NULL;
	server.connected = false;
	server.port = config::netplay_server_port;

	//Client info
	sender.host_socket = NULL;
	sender.connected = false;
	sender.port = config::netplay_client_port;

	#endif
}

/****** Tranfers one byte to another system ******/
bool DMG_SIO::send_byte()
{
	#ifdef GBE_NETPLAY

	u8 temp_buffer[2];
	temp_buffer[0] = sio_stat.transfer_byte;
	temp_buffer[1] = 0;

	if(SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2) < 2)
	{
		std::cout<<"SIO::Error - Host failed to send data to client\n";
		sio_stat.connected = false;
		server.connected = false;
		sender.connected = false;
		return false;
	}

	//Wait for other Game Boy to send this one its SB
	//This is blocking, will effectively pause GBE+ until it gets something
	if(SDLNet_TCP_Recv(server.remote_socket, temp_buffer, 2) > 0)
	{
		mem->memory_map[REG_SB] = sio_stat.transfer_byte = temp_buffer[0];
	}

	//Raise SIO IRQ after sending byte
	mem->memory_map[IF_FLAG] |= 0x08;

	#endif

	return true;
}

/****** Tranfers one bit to another system's IR port ******/
bool DMG_SIO::send_ir_signal()
{
	#ifdef GBE_NETPLAY

	u8 temp_buffer[2];

	//For IR signals, flag it properly
	//1st byte is IR signal data, second byte GBE+'s marker, 0x40
	temp_buffer[0] = mem->ir_signal;
	temp_buffer[1] = 0x40;

	if(SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2) < 2)
	{
		std::cout<<"SIO::Error - Host failed to send data to client\n";
		sio_stat.connected = false;
		server.connected = false;
		sender.connected = false;
		return false;
	}

	//Wait for other instance of GBE+ to send an acknowledgement
	//This is blocking, will effectively pause GBE+ until it gets something
	if(SDLNet_TCP_Recv(server.remote_socket, temp_buffer, 2) > 0)
	{
		mem->ir_send = false;
	}

	#endif

	return true;
}

/****** Receives one byte from another system ******/
bool DMG_SIO::receive_byte()
{
	#ifdef GBE_NETPLAY

	u8 temp_buffer[1];
	temp_buffer[0] = temp_buffer[1] = 0;

	//Check the status of connection
	SDLNet_CheckSockets(tcp_sockets, 0);

	//If this socket is active, receive the transfer
	if(SDLNet_SocketReady(server.remote_socket))
	{
		if(SDLNet_TCP_Recv(server.remote_socket, temp_buffer, 2) > 0)
		{
			//Stop sync
			if(temp_buffer[1] == 0xFF)
			{
				sio_stat.sync = false;
				sio_stat.sync_counter = 0;
				return true;
			}

			//Disconnect netplay
			else if(temp_buffer[1] == 0x80)
			{
				std::cout<<"SIO::Netplay connection terminated. Restart to reconnect.\n";
				sio_stat.connected = false;
				sio_stat.sync = false;
				return true;
			}

			//Receive IR signal
			else if(temp_buffer[1] == 0x40)
			{
				temp_buffer[1] = 0x41;
				
				//Clear out Bit 0 of RP if receiving signal
				if(temp_buffer[0] == 1) { mem->memory_map[REG_RP] &= ~0x2; }

				//Set Bit 1 of RP if IR signal is normal
				else { mem->memory_map[REG_RP] |= 0x2; }

				//Send acknowlegdement
				SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2);

				return true;
			}

			else if(temp_buffer[1] != 0) { return true; }

			//Raise SIO IRQ after sending byte
			mem->memory_map[IF_FLAG] |= 0x08;

			//Store byte from transfer into SB
			sio_stat.transfer_byte = mem->memory_map[REG_SB];
			mem->memory_map[REG_SB] = temp_buffer[0];

			//Reset Bit 7 of SC
			mem->memory_map[REG_SC] &= ~0x80;

			//Send other Game Boy the old SB value
			temp_buffer[0] = sio_stat.transfer_byte;
			sio_stat.transfer_byte = mem->memory_map[REG_SB];

			if(SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2) < 2)
			{
				std::cout<<"SIO::Error - Host failed to send data to client\n";
				sio_stat.connected = false;
				server.connected = false;
				sender.connected = false;
				return false;
			}
		}
	}

	#endif

	return true;
}

/****** Requests syncronization with another system ******/
bool DMG_SIO::request_sync()
{
	#ifdef GBE_NETPLAY

	u8 temp_buffer[2];
	temp_buffer[0] = 0;
	temp_buffer[1] = 0xFF;

	//Send the sync code 0xFF
	if(SDLNet_TCP_Send(sender.host_socket, (void*)temp_buffer, 2) < 2)
	{
		std::cout<<"SIO::Error - Host failed to send data to client\n";
		sio_stat.connected = false;
		server.connected = false;
		sender.connected = false;
		return false;
	}

	sio_stat.sync = true;

	#endif

	return true;
}

/****** Manages network communication via SDL_net ******/
void DMG_SIO::process_network_communication()
{
	#ifdef GBE_NETPLAY

	//If no communication with another GBE+ instance has been established yet, see if a connection can be made
	if(!sio_stat.connected)
	{
		//Try to accept incoming connections to the server
		if(!server.connected)
		{
			if(server.remote_socket = SDLNet_TCP_Accept(server.host_socket))
			{
				std::cout<<"SIO::Client connected\n";
				SDLNet_TCP_AddSocket(tcp_sockets, server.host_socket);
				SDLNet_TCP_AddSocket(tcp_sockets, server.remote_socket);
				server.connected = true;
			}
		}

		//Try to establish an outgoing connection to the server
		if(!sender.connected)
		{
			//Open a connection to listen on host's port
			if(sender.host_socket = SDLNet_TCP_Open(&sender.host_ip))
			{
				std::cout<<"SIO::Connected to server\n";
				SDLNet_TCP_AddSocket(tcp_sockets, sender.host_socket);
				sender.connected = true;
			}
		}

		if((server.connected) && (sender.connected))
		{
			sio_stat.connected = true;

			//Set the emulated SIO device type
			sio_stat.sio_type = (config::gb_type < 2) ? DMG_LINK : GBC_LINK;
		}
	}

	#endif
}

/****** Processes data sent to the GB Printer ******/
void DMG_SIO::printer_process()
{
	switch(printer.current_state)
	{
		//Receive packet data
		case GBP_AWAITING_PACKET:

			//Push data to packet buffer	
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;

			//Check for magic number 0x88 0x33
			if((printer.packet_size == 2) && (printer.packet_buffer[0] == 0x88) && (printer.packet_buffer[1] == 0x33))
			{
				//Move to the next state
				printer.current_state = GBP_RECEIVE_COMMAND;
			}

			//If magic number not found, reset
			else if(printer.packet_size == 2)
			{
				printer.packet_size = 1;
				u8 temp = printer.packet_buffer[1];
				printer.packet_buffer.clear();
				printer.packet_buffer.push_back(temp);
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;
			
			break;

		//Receive command
		case GBP_RECEIVE_COMMAND:

			//Push data to packet buffer
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;

			//Grab command. Check to see if the value is a valid command
			printer.command = printer.packet_buffer.back();

			//Abort if invalid command, wait for a new packet
			if((printer.command != 0x1) && (printer.command != 0x2) && (printer.command != 0x4) && (printer.command != 0xF))
			{
				std::cout<<"SIO::Warning - Invalid command sent to GB Printer -> 0x" << std::hex << (u32)printer.command << "\n";
				printer.current_state = GBP_AWAITING_PACKET;
			}

			else
			{
				//Move to the next state
				printer.current_state = GBP_RECEIVE_COMPRESSION_FLAG;
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Receive compression flag
		case GBP_RECEIVE_COMPRESSION_FLAG:

			//Push data to packet buffer
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;

			//Grab compression flag
			printer.compression_flag = printer.packet_buffer.back();

			//Move to the next state
			printer.current_state = GBP_RECEIVE_LENGTH;

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Receive data length
		case GBP_RECEIVE_LENGTH:

			//Push data to packet buffer
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;

			//Grab LSB of data length
			if(printer.packet_size == 5)
			{
				printer.data_length = 0;
				printer.data_length |= printer.packet_buffer.back();
			}

			//Grab MSB of the data length, move to the next state
			else if(printer.packet_size == 6)
			{
				printer.packet_size = 0;
				printer.data_length |= (printer.packet_buffer.back() << 8);
				
				//Receive data only if the length is non-zero
				if(printer.data_length > 0) { printer.current_state = GBP_RECEIVE_DATA; }

				//Otherwise, move on to grab the checksum
				else { printer.current_state = GBP_RECEIVE_CHECKSUM; }
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Receive data
		case GBP_RECEIVE_DATA:

			//Push data to packet buffer
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;

			//Once the specified amount of data is transferred, move to the next stage
			if(printer.packet_size == printer.data_length)
			{
				printer.packet_size = 0;
				printer.current_state = GBP_RECEIVE_CHECKSUM;
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Receive checksum
		case GBP_RECEIVE_CHECKSUM:

			//Push data to packet buffer
			printer.packet_buffer.push_back(sio_stat.transfer_byte);
			printer.packet_size++;
			
			//Grab LSB of checksum
			if(printer.packet_size == 1)
			{
				printer.checksum = 0;
				printer.checksum |= printer.packet_buffer.back();
			}

			//Grab MSB of the checksum, move to the next state
			else if(printer.packet_size == 2)
			{
				printer.packet_size = 0;
				printer.checksum |= (printer.packet_buffer.back() << 8);
				printer.current_state = GBP_ACKNOWLEDGE_PACKET;

				u16 checksum_match = 0;

				//Calculate checksum
				for(u32 x = 2; x < (printer.packet_buffer.size() - 2); x++)
				{
					checksum_match += printer.packet_buffer[x];
				}

				if(checksum_match != printer.checksum) { printer.status |= 0x1; }
				else { printer.status &= ~0x1; }
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x0;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Acknowledge packet and process command
		case GBP_ACKNOWLEDGE_PACKET:
		
			//GB Printer expects 2 0x0s, only continue if given them
			if(sio_stat.transfer_byte == 0)
			{
				//Push data to packet buffer
				printer.packet_buffer.push_back(sio_stat.transfer_byte);
				printer.packet_size++;

				//Send back 0x81 to GB + IRQ on 1st 0x0
				if(printer.packet_size == 1)
				{
					mem->memory_map[REG_SB] = 0x81;
					mem->memory_map[IF_FLAG] |= 0x08;
				}

				//Send back current status to GB + IRQ on 2nd 0x0, begin processing command
				else if(printer.packet_size == 2)
				{
					printer_execute_command();

					mem->memory_map[REG_SB] = printer.status;
					mem->memory_map[IF_FLAG] |= 0x08;

					printer.packet_buffer.clear();
					printer.packet_size = 0;
				}
			}

			break;
	}
}

/****** Executes commands send to the GB Printer ******/
void DMG_SIO::printer_execute_command()
{
	switch(printer.command)
	{
		//Initialize command
		case 0x1:
			printer.status = 0x0;
			printer.strip_count = 0;

			//Clear internal scanline data
			printer.scanline_buffer.clear();
			printer.scanline_buffer.resize(0x5A00, 0x0);
			
			break;

		//Print command
		case 0x2:
			print_image();
			printer.status = 0x4;

			break;

		//Data process command
		case 0x4:
			printer_data_process();
			
			//Only set Ready-To-Print status if some actual data was received
			if(printer.strip_count != 0) { printer.status = 0x8; }

			break;

		//Status command
		case 0xF:
			printer.status |= 0;

			break;

		default:
			std::cout<<"SIO::Warning - Invalid command sent to GB Printer -> 0x" << std::hex << (u32)printer.command << "\n";
			break;
	}

	printer.current_state = GBP_AWAITING_PACKET;
}

/****** Processes dot data sent to GB Printer ******/
void DMG_SIO::printer_data_process()
{
	u32 data_pointer = 6;
	u32 pixel_counter = printer.strip_count * 2560;
	u8 tile_pixel = 0;

	if(printer.strip_count >= 9)
	{
		for(u32 x = 0; x < 2560; x++) { printer.scanline_buffer.push_back(0x0); }	
	}

	//Process uncompressed dot data
	if(!printer.compression_flag)
	{
		//Cycle through all tiles given in the data, 40 in all
		for(u32 x = 0; x < 40; x++)
		{
			//Grab 16-bytes representing each tile, 2 bytes at a time
			for(u32 y = 0; y < 8; y++)
			{
				//Move pixel counter down one row in the tile
				pixel_counter = (printer.strip_count * 2560) + ((x % 20) * 8) + (160 * y);
				if(x >= 20) { pixel_counter += 1280; }

				//Grab 2-bytes representing 8x1 section
				u16 tile_data = (printer.packet_buffer[data_pointer + 1] << 8) | printer.packet_buffer[data_pointer];
				data_pointer += 2;

				//Determine color of each pixel in that 8x1 section
				for(int z = 7; z >= 0; z--)
				{
					//Calculate raw value of the tile's pixel
					tile_pixel = ((tile_data >> 8) & (1 << z)) ? 2 : 0;
					tile_pixel |= (tile_data & (1 << z)) ? 1 : 0;

					switch(tile_pixel)
					{
						case 0: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[0];
							break;

						case 1: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[1];
							break;

						case 2:
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[2];
							break;

						case 3: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[3];
							break;
					}
				}
			}
		}
	}

	//Process compressed dot data
	else
	{
		std::vector<u8> dot_data;
		u8 data = 0;

		//Cycle through all the compressed data and calculate the RLE
		while(data_pointer < (printer.data_length + 6))
		{
			//Grab MSB of 1st byte in the run, if 1 the run is compressed, otherwise it is an uncompressed run
			u8 data = printer.packet_buffer[data_pointer++];

			//Compressed run
			if(data & 0x80)
			{
				u8 length = (data & 0x7F) + 2;
				data = printer.packet_buffer[data_pointer++];

				for(u32 x = 0; x < length; x++) { dot_data.push_back(data); }
			}

			//Uncompressed run
			else
			{
				u8 length = (data & 0x7F) + 1;
				
				for(u32 x = 0; x < length; x++)
				{
					data = printer.packet_buffer[data_pointer++];
					dot_data.push_back(data);
				}
			}
		}

		data_pointer = 0;

		//Cycle through all tiles given in the data, 40 in all
		for(u32 x = 0; x < 40; x++)
		{
			//Grab 16-bytes representing each tile, 2 bytes at a time
			for(u32 y = 0; y < 8; y++)
			{
				//Move pixel counter down one row in the tile
				pixel_counter = (printer.strip_count * 2560) + ((x % 20) * 8) + (160 * y);
				if(x >= 20) { pixel_counter += 1280; }

				//Grab 2-bytes representing 8x1 section
				u16 tile_data = (dot_data[data_pointer + 1] << 8) | dot_data[data_pointer];
				data_pointer += 2;

				//Determine color of each pixel in that 8x1 section
				for(int z = 7; z >= 0; z--)
				{
					//Calculate raw value of the tile's pixel
					tile_pixel = ((tile_data >> 8) & (1 << z)) ? 2 : 0;
					tile_pixel |= (tile_data & (1 << z)) ? 1 : 0;

					switch(tile_pixel)
					{
						case 0: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[0];
							break;

						case 1: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[1];
							break;

						case 2:
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[2];
							break;

						case 3: 
							printer.scanline_buffer[pixel_counter++] = config::DMG_BG_PAL[3];
							break;
					}
				}
			}
		}
	}

	//Only increment strip count if we actually received data
	if(printer.data_length != 0) { printer.strip_count++; }
}

/****** Save GB Printer image to a BMP ******/
void DMG_SIO::print_image()
{
	u32 height = (16 * printer.strip_count);
	u32 img_size = 160 * height;

	//Set up printing palette
	u8 data_pal = printer.packet_buffer[8];

	printer.pal[0] = data_pal & 0x3;
	printer.pal[1] = (data_pal >> 2) & 0x3;
	printer.pal[2] = (data_pal >> 4) & 0x3;
	printer.pal[3] = (data_pal >> 6) & 0x3;

	srand(SDL_GetTicks());

	std::string filename = "gb_print_";
	filename += util::to_str(rand() % 1024);
	filename += util::to_str(rand() % 1024);
	filename += util::to_str(rand() % 1024);

	//Create a 160x144 image from the buffer, save as BMP
	SDL_Surface *print_screen = SDL_CreateRGBSurface(SDL_SWSURFACE, 160, height, 32, 0, 0, 0, 0);

	//Lock source surface
	if(SDL_MUSTLOCK(print_screen)){ SDL_LockSurface(print_screen); }
	u32* out_pixel_data = (u32*)print_screen->pixels;

	for(u32 x = 0; x < img_size; x++)
	{
		//Convert pixels to printer palette if necessary
		u8 tile_pixel = 0;
		
		if(printer.scanline_buffer[x] == config::DMG_BG_PAL[0]) { tile_pixel = 0; }
		else if(printer.scanline_buffer[x] == config::DMG_BG_PAL[1]) { tile_pixel = 1; }
		else if(printer.scanline_buffer[x] == config::DMG_BG_PAL[2]) { tile_pixel = 2; }
		else if(printer.scanline_buffer[x] == config::DMG_BG_PAL[3]) { tile_pixel = 3; }

		switch(printer.pal[tile_pixel])
		{
			case 0: 
				printer.scanline_buffer[x] = config::DMG_BG_PAL[0];
				break;

			case 1: 
				printer.scanline_buffer[x] = config::DMG_BG_PAL[1];
				break;

			case 2:
				printer.scanline_buffer[x] = config::DMG_BG_PAL[2];
				break;

			case 3: 
				printer.scanline_buffer[x] = config::DMG_BG_PAL[3];
				break;
		}
			
		out_pixel_data[x] = printer.scanline_buffer[x];
	}

	//Unlock source surface
	if(SDL_MUSTLOCK(print_screen)){ SDL_UnlockSurface(print_screen); }

	SDL_SaveBMP(print_screen, filename.c_str());
	SDL_FreeSurface(print_screen);

	printer.strip_count = 0;
}

/****** Processes data sent to the GB Mobile Adapter ******/
void DMG_SIO::mobile_adapter_process()
{
	//std::cout<<"CURRENT BYTE -> 0x" << std::hex << (u32)sio_stat.transfer_byte << "\n";

	switch(mobile_adapter.current_state)
	{
		//Receive packet data
		case GBMA_AWAITING_PACKET:
			
			//Push data to packet buffer	
			mobile_adapter.packet_buffer.push_back(sio_stat.transfer_byte);
			mobile_adapter.packet_size++;

			//Check for magic number 0x99 0x66
			if((mobile_adapter.packet_size == 2) && (mobile_adapter.packet_buffer[0] == 0x99) && (mobile_adapter.packet_buffer[1] == 0x66))
			{
				//Move to the next state
				mobile_adapter.packet_size = 0;
				mobile_adapter.current_state = GBMA_RECEIVE_HEADER;

				std::cout<<"SIO::Mobile Adapter - Magic Bytes Detected\n";
			}

			//If magic number not found, reset
			else if(mobile_adapter.packet_size == 2)
			{
				mobile_adapter.packet_size = 1;
				u8 temp = mobile_adapter.packet_buffer[1];
				mobile_adapter.packet_buffer.clear();
				mobile_adapter.packet_buffer.push_back(temp);
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x4B;
			mem->memory_map[IF_FLAG] |= 0x08;
			
			break;

		//Receive header data
		case GBMA_RECEIVE_HEADER:

			//Push data to packet buffer
			mobile_adapter.packet_buffer.push_back(sio_stat.transfer_byte);
			mobile_adapter.packet_size++;

			//Process header data
			if(mobile_adapter.packet_size == 4)
			{
				mobile_adapter.command = mobile_adapter.packet_buffer[2];
				mobile_adapter.data_length = mobile_adapter.packet_buffer[5];

				//Move to the next state
				mobile_adapter.packet_size = 0;
				mobile_adapter.current_state = (mobile_adapter.data_length == 0) ? GBMA_RECEIVE_CHECKSUM : GBMA_RECEIVE_DATA;

				std::cout<<"SIO::Mobile Adapter - Command ID 0x" << std::hex << (u32)mobile_adapter.command << "\n";
				std::cout<<"SIO::Mobile Adapter - Data Length 0x" << std::hex << (u32)mobile_adapter.data_length << "\n";
			}
			
			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x4B;
			mem->memory_map[IF_FLAG] |= 0x08;
			
			break;

		//Receive tranferred data
		case GBMA_RECEIVE_DATA:

			//Push data to packet buffer
			mobile_adapter.packet_buffer.push_back(sio_stat.transfer_byte);
			mobile_adapter.packet_size++;

			//Move onto the next state once all data has been received
			if(mobile_adapter.packet_size == mobile_adapter.data_length)
			{
				mobile_adapter.packet_size = 0;
				mobile_adapter.current_state = GBMA_RECEIVE_CHECKSUM;
			}

			//Send data back to GB + IRQ
			mem->memory_map[REG_SB] = 0x4B;
			mem->memory_map[IF_FLAG] |= 0x08;

			break;

		//Receive packet checksum
		case GBMA_RECEIVE_CHECKSUM:

			//Push data to packet buffer
			mobile_adapter.packet_buffer.push_back(sio_stat.transfer_byte);
			mobile_adapter.packet_size++;

			//Grab MSB of the checksum
			if(mobile_adapter.packet_size == 1)
			{
				mobile_adapter.checksum = (sio_stat.transfer_byte << 8);

				//Send data back to GB + IRQ
				mem->memory_map[REG_SB] = 0x4B;
				mem->memory_map[IF_FLAG] |= 0x08;
			}

			//Grab LSB of the checksum, move on to next state
			else if(mobile_adapter.packet_size == 2)
			{
				mobile_adapter.checksum |= sio_stat.transfer_byte;

				//Verify the checksum data
				u16 real_checksum = 0;

				for(u32 x = 2; x < (mobile_adapter.data_length + 6); x++)
				{
					real_checksum += mobile_adapter.packet_buffer[x];
				}

				//Move on to packet acknowledgement
				if(mobile_adapter.checksum == real_checksum)
				{
					mobile_adapter.packet_size = 0;
					mobile_adapter.current_state = GBMA_ACKNOWLEDGE_PACKET;

					//Send data back to GB + IRQ
					mem->memory_map[REG_SB] = 0x4B;
					mem->memory_map[IF_FLAG] |= 0x08;

					std::cout<<"SIO::Mobile Adapter - Checksum Clear \n";
				}

				//Send and error and wait for the packet to restart
				else
				{
					mobile_adapter.packet_size = 0;
					mobile_adapter.current_state = GBMA_AWAITING_PACKET;

					//Send data back to GB + IRQ
					mem->memory_map[REG_SB] = 0xF1;
					mem->memory_map[IF_FLAG] |= 0x08;

					std::cout<<"SIO::Mobile Adapter - Checksum Fail \n";
				}
			}

			break;

		//Acknowledge packet
		case GBMA_ACKNOWLEDGE_PACKET:
		
			//Push data to packet buffer
			mobile_adapter.packet_buffer.push_back(sio_stat.transfer_byte);
			mobile_adapter.packet_size++;

			//Mobile Adapter expects 0x80 from GBC, sends back 0x8C through 0x8F (does not matter which)
			if(mobile_adapter.packet_size == 1)
			{
				if(sio_stat.transfer_byte == 0x80)
				{
					mem->memory_map[REG_SB] = 0x8C;
					mem->memory_map[IF_FLAG] |= 0x08;
				}

				else
				{
					mobile_adapter.packet_size = 0;
					mem->memory_map[REG_SB] = 0x4B;
					mem->memory_map[IF_FLAG] |= 0x08;
				}	
			}

			//Send back 0x80 XOR current command + IRQ on 2nd byte, begin processing command
			else if(mobile_adapter.packet_size == 2)
			{
				std::cout<<"SIO::Mobile Adapter - Packet Size -> " << mobile_adapter.packet_buffer.size() << "\n";

				mem->memory_map[REG_SB] = 0x80 ^ mobile_adapter.command;
				mem->memory_map[IF_FLAG] |= 0x08;

				//Process commands sent to the mobile adapter
				switch(mobile_adapter.command)
				{

					//For certain commands, the mobile adapter echoes the packet it received
					case 0x10:
					case 0x11:
						mobile_adapter.packet_size = 0;
						mobile_adapter.current_state = GBMA_ECHO_PACKET;

						//Echo packet needs to have the proper handshake with the adapter ID and command
						mobile_adapter.packet_buffer[mobile_adapter.packet_buffer.size() - 2] = 0x8C;
						mobile_adapter.packet_buffer[mobile_adapter.packet_buffer.size() - 1] = 0x8C ^ mobile_adapter.command;
						
						break;

					//Read configuration data
					case 0x19:
						//Grab the offset and length to read. Two bytes of data
						if(mobile_adapter.data_length >= 2)
						{
							u8 config_offset = mobile_adapter.packet_buffer[6];
							u8 config_length = mobile_adapter.packet_buffer[7];

							//Start building the reply packet
							mobile_adapter.packet_buffer.clear();

							//Magic bytes
							mobile_adapter.packet_buffer.push_back(0x99);
							mobile_adapter.packet_buffer.push_back(0x66);

							//Header
							mobile_adapter.packet_buffer.push_back(0x19);
							mobile_adapter.packet_buffer.push_back(0x00);
							mobile_adapter.packet_buffer.push_back(0x00);
							mobile_adapter.packet_buffer.push_back(config_length);

							//Data from 192-byte configuration memory
							for(u32 x = config_offset; x < (config_offset + config_length); x++)
							{
								if(x < 0xC0) { mobile_adapter.packet_buffer.push_back(mobile_adapter.data[x]); }
								else { std::cout<<"SIO::Error - Mobile Adapter trying to read out-of-bounds memory\n"; return; }
							}

							//Checksum
							u16 checksum = 0;
							for(u32 x = 2; x < mobile_adapter.packet_buffer.size(); x++) { checksum += mobile_adapter.packet_buffer[x]; }

							mobile_adapter.packet_buffer.push_back((checksum >> 8) & 0xFF);
							mobile_adapter.packet_buffer.push_back(checksum & 0xFF);

							//Acknowledgement handshake
							mobile_adapter.packet_buffer.push_back(0x8C);
							mobile_adapter.packet_buffer.push_back(0x95);

							//Send packet back
							mobile_adapter.packet_size = 0;
							mobile_adapter.current_state = GBMA_ECHO_PACKET;
						}

						else { std::cout<<"SIO::Error - Mobile Adapter requested unspecified configuration data\n"; return; }

						break;

					//Write configuration data
					case 0x1A:
						{
							//Grab the offset and length to write. Two bytes of data
							u8 config_offset = mobile_adapter.packet_buffer[6];

							//Write data from the packet into memory configuration
							for(u32 x = 7; x < (mobile_adapter.data_length + 6); x++)
							{
								if(config_offset < 0xC0) { mobile_adapter.data[config_offset++] = mobile_adapter.packet_buffer[x]; }
								else { std::cout<<"SIO::Error - Mobile Adapter trying to write out-of-bounds memory\n"; return; }
							}

							//Start building the reply packet - Empty body
							mobile_adapter.packet_buffer.clear();

							//Magic bytes
							mobile_adapter.packet_buffer.push_back(0x99);
							mobile_adapter.packet_buffer.push_back(0x66);

							//Header
							mobile_adapter.packet_buffer.push_back(0x1A);
							mobile_adapter.packet_buffer.push_back(0x00);
							mobile_adapter.packet_buffer.push_back(0x00);
							mobile_adapter.packet_buffer.push_back(0x00);

							//Checksum
							u16 checksum = 0;
							for(u32 x = 2; x < mobile_adapter.packet_buffer.size(); x++) { checksum += mobile_adapter.packet_buffer[x]; }

							mobile_adapter.packet_buffer.push_back((checksum >> 8) & 0xFF);
							mobile_adapter.packet_buffer.push_back(checksum & 0xFF);

							//Acknowledgement handshake
							mobile_adapter.packet_buffer.push_back(0x8C);
							mobile_adapter.packet_buffer.push_back(0x96);

							//Send packet back
							mobile_adapter.packet_size = 0;
							mobile_adapter.current_state = GBMA_ECHO_PACKET;
						}
						
						break;

					default:
						std::cout<<"SIO::Mobile Adapter - Unknown Command ID 0x" << std::hex << (u32)mobile_adapter.command << "\n";
						SDL_Delay(3000);
						mobile_adapter.packet_buffer.clear();
						mobile_adapter.packet_size = 0;
						mobile_adapter.current_state = GBMA_AWAITING_PACKET;

						break;
				}

				std::cout<<"SIO::Mobile Adapter - Receive Done\n";
			}

			break;

		//Echo packet back to Game Boy
		case GBMA_ECHO_PACKET:
		
			//Send back the packet bytes
			if(mobile_adapter.packet_size < mobile_adapter.packet_buffer.size())
			{
				mem->memory_map[REG_SB] = mobile_adapter.packet_buffer[mobile_adapter.packet_size++];
				mem->memory_map[IF_FLAG] |= 0x08;

				if(mobile_adapter.packet_size == mobile_adapter.packet_buffer.size())
				{
					std::cout<<"SIO::Mobile Adapter - Echo Done\n";
			
					mobile_adapter.packet_buffer.clear();
					mobile_adapter.packet_size = 0;
					mobile_adapter.current_state = GBMA_AWAITING_PACKET;
				}
			}

			break;
	}
}

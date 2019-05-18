#include "pch.h"

#define NOMINMAX
#include <Windows.h>

#include "Test/gtest-cout.h"

#include "SimFramework/Framework.h"

#include "HostComm.hpp"
#include "SimpleFtl/Translation.h"
#include "SsdSimApp.h"

using namespace HostCommTest;

class SimpleFtlTest : public ::testing::Test
{
protected:
	void SetUp() override 
	{
		ASSERT_NO_THROW(SimFramework.Init("Hardwareconfig/hardwaremin.json"));

		FrameworkFuture = std::async(std::launch::async, &(Framework::operator()), &SimFramework);

		std::this_thread::sleep_for(std::chrono::milliseconds(1000));

		// TODO: implement way to get server name
		constexpr char* customMessagingName = "SsdSimCustomProtocolServer";
		CustomProtocolClient = std::make_shared<MessageClient<CustomProtocolCommand>>(customMessagingName);
		ASSERT_NE(nullptr, CustomProtocolClient);

		//Load dll SimpleFtl by sending command DownloadAndExecute
		constexpr char* simpleFtlDll = "SimpleFtl.dll";
		auto messageDownloadAndExecute = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, 0, false);
		ASSERT_NE(messageDownloadAndExecute, nullptr);
		messageDownloadAndExecute->Data.Command = CustomProtocolCommand::Code::DownloadAndExecute;
		memcpy(messageDownloadAndExecute->Data.Descriptor.DownloadAndExecute.CodeName, simpleFtlDll,
			sizeof(messageDownloadAndExecute->Data.Descriptor.DownloadAndExecute.CodeName));
		CustomProtocolClient->Push(messageDownloadAndExecute);

		//Get device info
		auto messageGetDeviceInfo = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, 0, true);
		ASSERT_NE(messageGetDeviceInfo, nullptr);
		messageGetDeviceInfo->Data.Command = CustomProtocolCommand::Code::GetDeviceInfo;
		CustomProtocolClient->Push(messageGetDeviceInfo);
		while (!CustomProtocolClient->HasResponse());
		DeviceInfoResponse = CustomProtocolClient->PopResponse();
	}

	void TearDown() override
	{
		CustomProtocolClient->DeallocateMessage(DeviceInfoResponse);

		//TODO: implement way to get server name
		constexpr char* messagingName = "SsdSimMainMessageServer";	//TODO: define a way to get name
		auto client = std::make_shared<SimFrameworkMessageClient>(messagingName);
		ASSERT_NE(nullptr, client);

		auto message = AllocateMessage<SimFrameworkCommand>(client, 0, false);
		ASSERT_NE(message, nullptr);
		message->Data.Code = SimFrameworkCommand::Code::Exit;
		client->Push(message);

		//Give the Framework a chance to stop completely before next test
		// This is a work around until multiple servers can be created without collision
		std::this_thread::sleep_for(std::chrono::milliseconds(3000));
	}

	Framework SimFramework;
	std::future<void> FrameworkFuture;
	CustomProtocolMessageClientSharedPtr CustomProtocolClient;
	CustomProtocolMessage* DeviceInfoResponse;
};

TEST(SimpleFtl, Translation_LbaToNand)
{
    NandHal::Geometry geometry;
    geometry._ChannelCount = 4;
    geometry._DevicesPerChannel = 2;
    geometry._BlocksPerDevice = 128;
    geometry._PagesPerBlock = 256;
    geometry._BytesPerPage = 8192;

    U32 lba = 0;
    NandHal::CommandDesc cmdDesc;

    SimpleFtlTranslation::SetGeometry(geometry);
    for (U32 block(0); block < geometry._BlocksPerDevice; ++block)
    {
        for (U32 page(0); page < geometry._PagesPerBlock; ++page)
        {
            for (U32 device(0); device < geometry._DevicesPerChannel; ++device)
            {
                for (U32 channel(0); channel < geometry._ChannelCount; ++channel)
                {
                    SimpleFtlTranslation::LbaToNandAddress(lba, cmdDesc.Address);
                    ASSERT_EQ(cmdDesc.Address.Channel._, channel);
                    ASSERT_EQ(cmdDesc.Address.Device._, device);
                    ASSERT_EQ(cmdDesc.Address.Page._, page);
                    ASSERT_EQ(cmdDesc.Address.Block._, block);
                    lba += (geometry._BytesPerPage >> 9);
                }
            }
        }
    }
}

TEST(SimpleFtl, BasicWriteReadVerify_App)
{
    //Start the app
    GOUT("Starting SsdSim process.");
    SHELLEXECUTEINFO ShExecInfo = { 0 };
    ASSERT_TRUE(LaunchProcess("SsdSim.exe", "--hardwarespec Hardwareconfig/hardwarespec.json", ShExecInfo));

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    DWORD exitCode = 0;
    ASSERT_TRUE(GetExitCodeProcess(ShExecInfo.hProcess, &exitCode));
    ASSERT_EQ(STILL_ACTIVE, exitCode);

    //TODO: implement way to get server name
    constexpr char* customMessagingName = "SsdSimCustomProtocolServer";
    auto clientCustomProtocolCmd = std::make_shared<MessageClient<CustomProtocolCommand>>(customMessagingName);
    ASSERT_NE(nullptr, clientCustomProtocolCmd);

    //Load dll SimpleFtl by sending command DownloadAndExecute
    constexpr char* simpleFtlDll = "SimpleFtl.dll";
    auto messageDownloadAndExecute = AllocateMessage<CustomProtocolCommand>(clientCustomProtocolCmd, 0, false);
    ASSERT_NE(messageDownloadAndExecute, nullptr);
    messageDownloadAndExecute->Data.Command = CustomProtocolCommand::Code::DownloadAndExecute;
    memcpy(messageDownloadAndExecute->Data.Descriptor.DownloadAndExecute.CodeName, simpleFtlDll,
        sizeof(messageDownloadAndExecute->Data.Descriptor.DownloadAndExecute.CodeName));
    clientCustomProtocolCmd->Push(messageDownloadAndExecute);

    //Get device info
    auto messageGetDeviceInfo = AllocateMessage<CustomProtocolCommand>(clientCustomProtocolCmd, 0, true);
    ASSERT_NE(messageGetDeviceInfo, nullptr);
    messageGetDeviceInfo->Data.Command = CustomProtocolCommand::Code::GetDeviceInfo;
    clientCustomProtocolCmd->Push(messageGetDeviceInfo);

    while (!clientCustomProtocolCmd->HasResponse());
    auto responseGetDeviceInfo = clientCustomProtocolCmd->PopResponse();

    //Write a buffer with lba and sector count
    constexpr U32 lba = 123455;
    U32 sectorCount = 35;
    U32 payloadSize = sectorCount * responseGetDeviceInfo->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
    auto writeMessage = AllocateMessage<CustomProtocolCommand>(clientCustomProtocolCmd, payloadSize, true);
    ASSERT_NE(writeMessage, nullptr);
    ASSERT_NE(writeMessage->Payload, nullptr);
    for (auto i(0); i < sectorCount; ++i)
    {
        auto buffer = &(static_cast<U8*>(writeMessage->Payload)[i * responseGetDeviceInfo->Data.Descriptor.DeviceInfoPayload.BytesPerSector]);
        memset((void*)buffer, lba + i, responseGetDeviceInfo->Data.Descriptor.DeviceInfoPayload.BytesPerSector);
    }
    writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
    writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
    writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
    clientCustomProtocolCmd->Push(writeMessage);

    //Wait for write command response
    while (!clientCustomProtocolCmd->HasResponse());
    auto responseMessageWrite = clientCustomProtocolCmd->PopResponse();

    //Read a buffer with lba and sector count
    auto readMessage = AllocateMessage<CustomProtocolCommand>(clientCustomProtocolCmd, payloadSize, true);
    ASSERT_NE(readMessage, nullptr);
    readMessage->Data.Command = CustomProtocolCommand::Code::Read;
    readMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
    readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
    clientCustomProtocolCmd->Push(readMessage);

    //Wait for read command response
    while (!clientCustomProtocolCmd->HasResponse());
    auto responseMessageRead = clientCustomProtocolCmd->PopResponse();

    //Get message read buffer to verify with the write buffer
    int compareResult = std::memcmp(responseMessageWrite->Payload, responseMessageRead->Payload, payloadSize);

    //--Deallocate
    clientCustomProtocolCmd->DeallocateMessage(responseGetDeviceInfo);
    clientCustomProtocolCmd->DeallocateMessage(responseMessageWrite);
    clientCustomProtocolCmd->DeallocateMessage(responseMessageRead);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    //TODO: implement way to get server name
    constexpr char* messagingName = "SsdSimMainMessageServer";	//TODO: define a way to get name
    auto client = std::make_shared<MessageClient<SimFrameworkCommand>>(messagingName);
    ASSERT_NE(nullptr, client);

    auto message = AllocateMessage<SimFrameworkCommand>(client, 0, false);
    ASSERT_NE(message, nullptr);
    message->Data.Code = SimFrameworkCommand::Code::Exit;
    client->Push(message);

    if (ShExecInfo.hProcess)
    {
        GOUT("Waiting for process termination.");
        ASSERT_NE(WAIT_TIMEOUT, WaitForSingleObject(ShExecInfo.hProcess, 10000));
        GetExitCodeProcess(ShExecInfo.hProcess, &exitCode);
        CloseHandle(ShExecInfo.hProcess);
    }

    ASSERT_EQ(0, compareResult);
}

TEST_F(SimpleFtlTest, BasicWriteReadVerify)
{
	U32 totalSector = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.TotalSector;

	//Write a buffer with lba and sector count
    constexpr U32 lba = 12345;
    U32 sectorCount = 35;
    U32 payloadSize = sectorCount * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
    auto writeMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(writeMessage, nullptr);
    ASSERT_NE(writeMessage->Payload, nullptr);
    for (auto i(0); i < sectorCount; ++i)
    {
        auto buffer = &(static_cast<U8*>(writeMessage->Payload)[i * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector]);
        memset((void*)buffer, lba + i, DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector);
    }
    writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
    writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
    writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
	CustomProtocolClient->Push(writeMessage);

    //Wait for write command response
    while (!CustomProtocolClient->HasResponse());
    auto responseMessageWrite = CustomProtocolClient->PopResponse();

	//Read a buffer with lba and sector count
    auto readMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(readMessage, nullptr);
    readMessage->Data.Command = CustomProtocolCommand::Code::Read;
    readMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
    readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
	CustomProtocolClient->Push(readMessage);

    //Wait for read command response
    while (!CustomProtocolClient->HasResponse());
    auto responseMessageRead = CustomProtocolClient->PopResponse();

    //Get message read buffer to verify with the write buffer
    int compareResult = std::memcmp(responseMessageWrite->Payload, responseMessageRead->Payload, payloadSize);

    //--Deallocate
	CustomProtocolClient->DeallocateMessage(responseMessageWrite);
	CustomProtocolClient->DeallocateMessage(responseMessageRead);
}

TEST_F(SimpleFtlTest, BasicAscendingWriteReadVerifyAll)
{
    U32 totalSector = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.TotalSector;

    constexpr U32 maxSectorPerTransfer = 256;
    U32 payloadSize = maxSectorPerTransfer * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
    auto writeMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(writeMessage, nullptr);
    ASSERT_NE(writeMessage->Payload, nullptr);
    auto readMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(readMessage, nullptr);

    //Write and read to verify all written data in ascending order
    U32 sectorCount;
    for (U32 lba(0); lba < totalSector; lba += maxSectorPerTransfer)
    {
        sectorCount = std::min(maxSectorPerTransfer, totalSector - lba);
        //Fill write buffer
        for (U32 i(0); i < payloadSize; ++i)
        {
            ((U8*)writeMessage->Payload)[i] = i % 255;
        }
        writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
        writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        CustomProtocolClient->Push(writeMessage);

        //Wait for write command response
        while (!CustomProtocolClient->HasResponse());
        auto responseMessageWrite = CustomProtocolClient->PopResponse();

        readMessage->Data.Command = CustomProtocolCommand::Code::Read;
        readMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        CustomProtocolClient->Push(readMessage);

        //Wait for read command response
        while (!CustomProtocolClient->HasResponse());
        auto responseMessageRead = CustomProtocolClient->PopResponse();

        //Get message read buffer to verify with the write buffer
        auto pReadData = CustomProtocolClient->GetMessage(responseMessageRead->Id())->Payload;
        auto result = std::memcmp(responseMessageWrite->Payload, pReadData,
            sectorCount * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector);
        ASSERT_EQ(0, result);
    }
}

TEST_F(SimpleFtlTest, BasicDescendingWriteReadVerifyAll)
{
    U32 totalSector = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.TotalSector;

    constexpr U32 maxSectorPerTransfer = 256;
    U32 payloadSize = maxSectorPerTransfer * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
    auto writeMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(writeMessage, nullptr);
    ASSERT_NE(writeMessage->Payload, nullptr);
    auto readMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
    ASSERT_NE(readMessage, nullptr);

    //Write and read to verify all written data in descending order
    U32 sectorCount;
    for (U32 lba(totalSector); lba > 0; )
    {
        if (lba > maxSectorPerTransfer)
        {
            lba = lba - maxSectorPerTransfer;
            sectorCount = maxSectorPerTransfer;
        }
        else
        {
            sectorCount = lba;
            lba = 0;
        }
        //Fill write buffer
        for (U32 i(0); i < payloadSize; ++i)
        {
            ((U8*)writeMessage->Payload)[i] = i % 255;
        }
        writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
        writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        CustomProtocolClient->Push(writeMessage);

        //Wait for write command response
        while (!CustomProtocolClient->HasResponse());
        auto responseMessageWrite = CustomProtocolClient->PopResponse();

        readMessage->Data.Command = CustomProtocolCommand::Code::Read;
        readMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        CustomProtocolClient->Push(readMessage);

        //Wait for read command response
        while (!CustomProtocolClient->HasResponse());
        auto responseMessageRead = CustomProtocolClient->PopResponse();

        //Get message read buffer to verify with the write buffer
        auto pReadData = CustomProtocolClient->GetMessage(responseMessageRead->Id())->Payload;
        auto result = std::memcmp(responseMessageWrite->Payload, pReadData,
            sectorCount * DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector);
        ASSERT_EQ(0, result);
    }
}

TEST_F(SimpleFtlTest, BasicWriteReadBenchmark)
{
    using namespace std::chrono;

    constexpr U32 payloadSize = 128 * 1024;
    constexpr U32 commandCount = 10;
    constexpr U32 lba = 0;
    constexpr U32 sectorCount = 256;
    Message<CustomProtocolCommand>* messages[commandCount];
    for (auto i = 0; i < commandCount; ++i)
    {
        messages[i] = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
        ASSERT_NE(messages[i], nullptr);
    }

    unsigned long totalBytesWrittenInSeconds = 0;
    double writeRate = 0;
    duration<double> dataCmdTotalTime = duration<double>::zero();
    std::map<U32, high_resolution_clock::time_point> t0s;
    for (auto i = 0; i < commandCount; ++i)
    {
        messages[i]->Data.Command = CustomProtocolCommand::Code::Write;
        messages[i]->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        messages[i]->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        memset(messages[i]->Payload, 0xaa, messages[i]->PayloadSize);
		t0s.insert(std::make_pair(messages[i]->Id(), high_resolution_clock::now()));
        CustomProtocolClient->Push(messages[i]);
    }

    U32 responseReceivedCount = 0;
    while (responseReceivedCount < commandCount)
    {
        if (CustomProtocolClient->HasResponse())
        {
            messages[responseReceivedCount] = CustomProtocolClient->PopResponse();

            auto deltaT = duration_cast<duration<double>>(high_resolution_clock::now() - t0s.find(messages[responseReceivedCount]->Id())->second);
            dataCmdTotalTime += deltaT;
            totalBytesWrittenInSeconds += messages[responseReceivedCount]->PayloadSize;

            responseReceivedCount++;
        }
    }
    writeRate = (double)(totalBytesWrittenInSeconds / 1024 / 1024) / dataCmdTotalTime.count();

    //--Read in benchmark
    unsigned long totalBytesReadInSeconds = 0;
    double readRate = 0;
    dataCmdTotalTime = duration<double>::zero();
    t0s.clear();
    for (auto i = 0; i < commandCount; ++i)
    {
        messages[i]->Data.Command = CustomProtocolCommand::Code::Read;
        messages[i]->Data.Descriptor.SimpleFtlPayload.Lba = lba;
        messages[i]->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
        t0s.insert(std::make_pair(messages[i]->Id(), high_resolution_clock::now()));
        CustomProtocolClient->Push(messages[i]);
    }

    responseReceivedCount = 0;
    while (responseReceivedCount < commandCount)
    {
        if (CustomProtocolClient->HasResponse())
        {
            messages[responseReceivedCount] = CustomProtocolClient->PopResponse();

            auto deltaT = duration_cast<duration<double>>(high_resolution_clock::now() - t0s.find(messages[responseReceivedCount]->Id())->second);
            dataCmdTotalTime += deltaT;
            totalBytesReadInSeconds += messages[responseReceivedCount]->PayloadSize;

            responseReceivedCount++;
        }
    }
    readRate = (double)(totalBytesReadInSeconds / 1024 / 1024) / dataCmdTotalTime.count();

    GOUT("Write/Read benchmark");
    GOUT("   Write rate: " << writeRate << " MB/s");
    GOUT("   Read rate: " << readRate << " MB/s");

    //--Deallocate
    for (auto i = 0; i < commandCount; ++i)
    {
        CustomProtocolClient->DeallocateMessage(messages[i]);
    }
}

TEST_F(SimpleFtlTest, BasicUnalignedWriteAlignedRead)
{
	U32 bytesPerSector = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
	U8 sectorsPerPage = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.SectorsPerPage;

	ASSERT_EQ(sectorsPerPage >= 4, true);	//this is for this specific test setup

	U32 lba = sectorsPerPage / 2;
	U32 sectorCount = sectorsPerPage / 4;

	auto writeMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, sectorCount * bytesPerSector, true);
	ASSERT_NE(writeMessage, nullptr);
	ASSERT_NE(writeMessage->Payload, nullptr);
	writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
	writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
	writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
	for (auto i(0); i < sectorCount; ++i)
	{
		auto buffer = &(static_cast<U8*>(writeMessage->Payload)[i * bytesPerSector]);
		memset((void*)buffer, lba + i, bytesPerSector);
	}
	CustomProtocolClient->Push(writeMessage);
	while (!CustomProtocolClient->HasResponse());
	auto writeMessageReponse = CustomProtocolClient->PopResponse();
	ASSERT_EQ(writeMessage, writeMessageReponse);

	auto readMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, bytesPerSector * sectorsPerPage, true);
	ASSERT_NE(readMessage, nullptr);
	readMessage->Data.Command = CustomProtocolCommand::Code::Read;
	readMessage->Data.Descriptor.SimpleFtlPayload.Lba = 0;
	readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorsPerPage;
	CustomProtocolClient->Push(readMessage);
	while (!CustomProtocolClient->HasResponse());
	auto readMessageReponse = CustomProtocolClient->PopResponse();
	ASSERT_EQ(readMessage, readMessageReponse);

	auto writeMessageBuffer = static_cast<void*>(writeMessageReponse->Payload);
	auto readMessageBuffer = &(static_cast<U8*>(readMessageReponse->Payload)[lba * bytesPerSector]);
	auto result = memcmp((void*)readMessageBuffer, writeMessageBuffer, sectorCount * bytesPerSector);
	ASSERT_EQ(0, result);
}

//! Sends the following IO command sequence
/*
	Write 0:256
	Read verify 0:256
	Write 0:256
	Read verify 0:256
*/
TEST_F(SimpleFtlTest, BasicRepeatedWriteReadVerify)
{
	constexpr U32 lba = 0;
	constexpr U32 sectorCount = 256;
	U32 bytesPerSector = DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.BytesPerSector;
	U32 payloadSize = sectorCount * bytesPerSector;

	ASSERT_EQ(DeviceInfoResponse->Data.Descriptor.DeviceInfoPayload.TotalSector >= sectorCount, true);

	auto writeMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
	ASSERT_NE(writeMessage, nullptr);
	ASSERT_NE(writeMessage->Payload, nullptr);
	writeMessage->Data.Command = CustomProtocolCommand::Code::Write;
	writeMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
	writeMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
	CustomProtocolMessage* writeMessageReponse;

	auto readMessage = AllocateMessage<CustomProtocolCommand>(CustomProtocolClient, payloadSize, true);
	ASSERT_NE(readMessage, nullptr);
	readMessage->Data.Command = CustomProtocolCommand::Code::Read;
	readMessage->Data.Descriptor.SimpleFtlPayload.Lba = lba;
	readMessage->Data.Descriptor.SimpleFtlPayload.SectorCount = sectorCount;
	CustomProtocolMessage* readMessageReponse;

	constexpr auto repeatCount = 2;
	U8 bytePattern[repeatCount] = { 0xa5, 0xff };
	for (auto loop(0); loop < repeatCount; ++loop)
	{
		for (U32 i(0); i < payloadSize; ++i)
		{
			((U8*)writeMessage->Payload)[i] = bytePattern[loop];
		}

		CustomProtocolClient->Push(writeMessage);
		while (!CustomProtocolClient->HasResponse());
		writeMessageReponse = CustomProtocolClient->PopResponse();
		ASSERT_EQ(writeMessage, writeMessageReponse);

		CustomProtocolClient->Push(readMessage);
		while (!CustomProtocolClient->HasResponse());
		readMessageReponse = CustomProtocolClient->PopResponse();
		ASSERT_EQ(readMessage, readMessageReponse);

		auto result = std::memcmp(writeMessage->Payload, readMessageReponse->Payload, payloadSize);
		ASSERT_EQ(0, result);
	}

	CustomProtocolClient->DeallocateMessage(writeMessage);
	CustomProtocolClient->DeallocateMessage(readMessage);
}
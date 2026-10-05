import { photoAccessHelper } from '@kit.MediaLibraryKit';
import { common } from '@kit.AbilityKit';

/**
 * 保存视频到系统相册（媒体库）。
 * 使用 MediaAssetChangeRequest（API 12+），首次保存由系统弹窗向用户授权。
 */
export class GalleryService {
  static async saveVideo(context: common.Context, filePath: string): Promise<string> {
    const helper = photoAccessHelper.getPhotoAccessHelper(context);
    const request = await photoAccessHelper.MediaAssetChangeRequest.createVideoAssetRequest(
      context, 'file://' + filePath);
    await helper.applyChanges(request);
    const asset = request.getAsset();
    return asset.uri;
  }
}

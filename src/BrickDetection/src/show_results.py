import cv2
import numpy as np
import utilities as utils

path_to_images = "images/"
state, image0 = utils.loadImage(path_to_images + "result_cropped")

state, image1 = utils.loadImage(path_to_images +"result_red_rgb")
state, image2 = utils.loadImage(path_to_images +"result_yellow_rgb")
state, image3 = utils.loadImage(path_to_images +"result_blue_rgb")

state, image4 = utils.loadImage(path_to_images +"result_red_hsv")
state, image5 = utils.loadImage(path_to_images +"result_yellow_hsv")
state, image6 = utils.loadImage(path_to_images +"result_blue_hsv")

RGB = np.hstack([image1, image2,image3])
HSV = np.hstack([image4, image5,image6])

original = utils.addPaddingToMatchSize(image0,np.vstack([image0,image0]))
# show the images
cv2.imshow("Result of applying color segmentation",
           np.hstack([original,np.vstack([RGB,HSV])]))
cv2.waitKey(0)
